/* SPDX-License-Identifier: GPL-2.0-only
 *
 * This implementation is derived from the ESP hosted project available at
 * https://github.com/espressif/esp-hosted which is
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 */

/*
 * Implementation notes
 * --------------------
 *
 * The driver features a work queue on which any device state change and transfer
 * is serialized. Care should be taken not to dead-lock with the HCI core
 * work queues. Generally, HCI calls are non-blocking, but for example
 * hci_unregister_dev() will sync the HCI work queues. Device state is tracked
 * by esp_hci_dev::dev_state.
 *
 * When entering the firmware download mode, we de-register the HCI core device.
 * This will implicitly set the device state to ESP_HCI_DEV_STATE_CLOSED. When
 * we exit firmware download mode, the HCI core device is registered, which
 * will trigger an automatic re-opening of the device. We always free the HCI
 * core device and create a new one because re-registering a previously
 * unregistered device just doesn't work. This state change is tracked by
 * esp_hci_dev::drv_state. Any changes to this state are triggered at
 * probe/remove or by the firmware update file, which serializes all its
 * operations.
 */
#include <linux/of.h>
#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/mod_devicetable.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "esp_hci.h"

#define ESP_IF_TYPE_HCI_HCI 2

/* Versioning for the framing between host and controller. The major versions
 * MUST match. */
#define ESP_HCI_FRAMING_VER_MAJOR 2
#define ESP_HCI_FRAMING_VER_MINOR 1
#define ESP_HCI_FRAMING_VER_PATCH 0

/* Only for fw dev management, file operations are serialized with dev-specific
 * lock. */
DEFINE_MUTEX(_fwdev_mgmt_lock);

#define ESP_HCI_MINOR_CNT 4

#define ESP_HCI_SPI_TX_QLEN_MAX 32
#define ESP_HCI_SPI_TX_QLEN_RESUME (ESP_HCI_SPI_TX_QLEN_MAX - 8)

static unsigned long _fwdev_map;
static unsigned _fwdev_major;
static struct class *_fwdev_class;

enum ESP_CAPABILITIES {
	ESP_BLE_ONLY_SUPPORT = (1 << 3),
	ESP_BR_EDR_ONLY_SUPPORT = (1 << 4),
	ESP_BT_SUPPORT = ESP_BLE_ONLY_SUPPORT | ESP_BR_EDR_ONLY_SUPPORT,
};

enum ESP_INTERFACE_TYPE {
	ESP_HCI_IF = 2,
	ESP_INTERNAL_IF,
};

enum ESP_BOOTUP_TAG_TYPE {
	ESP_BOOTUP_CAPABILITY = 0,
	ESP_BOOTUP_FRAMING_VER,
	ESP_BOOTUP_SPI_CLK_MHZ,
	ESP_BOOTUP_FIRMWARE_CHIP_ID,
	ESP_BOOTUP_FW_VER,
};

enum ESP_INTERNAL_MSG {
	ESP_INTERNAL_BOOTUP_EVENT = 1,
};

struct event_header {
	uint8_t event_code;
	uint8_t status;
	uint16_t len;
} __packed;

struct esp_internal_bootup_event {
	struct event_header header;
	uint8_t len;
	uint8_t pad[3];
	uint8_t data[0];
} __packed;

struct esp_cap_tag {
	uint8_t id;
	uint8_t len;
	uint8_t data[0];
} __packed;

static int _send_packet(struct esp_hci_dev *esp_hci_dev, struct sk_buff *skb);
static void _flush_tx_queue(struct esp_hci_dev *esp_hci_dev);

static void esp_hci_open_work(struct work_struct *work);
static void esp_hci_close_work(struct work_struct *work);
static int esp_hci_open(struct hci_dev *hdev);
static int esp_hci_flush(struct hci_dev *hdev);
static int esp_hci_close(struct hci_dev *hdev);
static void esp_hci_reset(struct hci_dev *hdev);
static int esp_hci_send(struct hci_dev *hdev, struct sk_buff *skb);

static void _state_change(struct esp_hci_dev *esp_hci_dev, esp_hci_dev_state_t new)
{
	esp_hci_dev->dev_state = new;
	wake_up_all(&esp_hci_dev->dev_state_change);
}

static void __maybe_unused _debug_header(struct esp_payload_header const *hdr,
					 char const *dir)
{
	printk_ratelimited("%s HDR:\n"
			   " if_type     = %u\n"
			   " seq         = %u\n"
			   " reserved    = %u\n"
			   " len         = %u\n"
			   " offset      = %u\n"
			   " checksum    = %u\n"
			   " reserved2   = %u\n",
			   dir, hdr->if_type, hdr->seq, hdr->reserved, hdr->len,
			   hdr->offset, hdr->checksum, hdr->reserved2);

	print_hex_dump(KERN_INFO, dir, DUMP_PREFIX_ADDRESS, 16, 1, hdr, 64,
		       true);
}

/* For some reason, unregistering and registering will not work, I guess this
 * just off the beaten path. So, we always init clean. */
static int _register_hci_dev(struct esp_hci_dev *esp_hci_dev)
{
	if (esp_hci_dev->drv_state == ESP_HCI_DRV_STATE_REG) {
		dev_warn(esp_hci_dev->transport_dev, "HCI: HCI dev already registered");
		return 0;
	}

	BUG_ON(esp_hci_dev->hci_dev);

	struct hci_dev *hci_dev = hci_alloc_dev();
	if (!hci_dev) {
		dev_err(esp_hci_dev->transport_dev, "HCI: hci_alloc_dev err\n");
		return -ENOMEM;
	}

	esp_hci_dev->hci_dev = hci_dev;

	hci_set_drvdata(hci_dev, esp_hci_dev);

	hci_dev->bus = esp_hci_dev->type;
	hci_dev->close = esp_hci_close;
	hci_dev->open = esp_hci_open;
	hci_dev->send = esp_hci_send;
	hci_dev->reset = esp_hci_reset;
	hci_dev->flush = esp_hci_flush;

	int res = hci_register_dev(esp_hci_dev->hci_dev);
	if (res < 0) {
		hci_free_dev(hci_dev);
		esp_hci_dev->hci_dev = NULL;
		dev_err(esp_hci_dev->transport_dev,
			"HCI: cannot register HCI dev: %d\n", res);
		return res;
	}

	dev_info(esp_hci_dev->transport_dev, "HCI: registered HCI dev\n");
	esp_hci_dev->drv_state = ESP_HCI_DRV_STATE_REG;

	return 0;
}

static void _unregister_hci_dev(struct esp_hci_dev *esp_hci_dev)
{
	if (esp_hci_dev->drv_state < ESP_HCI_DRV_STATE_REG) {
		dev_warn(esp_hci_dev->transport_dev, "HCI: HCI dev already unregistered");
		return;
	}

	/* Oops if the HCI dev was already unregistered. */
	hci_unregister_dev(esp_hci_dev->hci_dev);
	dev_info(esp_hci_dev->transport_dev, "HCI: unregistered HCI dev\n");

	flush_workqueue(esp_hci_dev->wq);

	hci_free_dev(esp_hci_dev->hci_dev);
	esp_hci_dev->hci_dev = NULL;

	esp_hci_dev->drv_state = ESP_HCI_DRV_STATE_UNREG;
}

static void _power_on_reset(struct esp_hci_dev *esp_hci_dev)
{
	/* If power pin is not supported, trigger a reset to put the controller
	 * in a clean state. Otherwise this won't do anything, as the dev should
	 * be off.*/
	gpiod_set_value(esp_hci_dev->rst_gpio, 1);
	msleep(3);
	gpiod_set_value(esp_hci_dev->rst_gpio, 0);

	gpiod_set_value(esp_hci_dev->pwr_gpio, 1);
}

static void esp_hci_open_work(struct work_struct *work)
{
	struct esp_hci_dev *esp_hci_dev = ((struct esp_hci_work *)work)->esp_hci_dev;
	gpiod_set_value(esp_hci_dev->pwr_gpio, 0);

	_power_on_reset(esp_hci_dev);

	_state_change(esp_hci_dev, ESP_HCI_DEV_STATE_OPENING);
}

static void esp_hci_close_work(struct work_struct *work)
{
	struct esp_hci_dev *esp_hci_dev = ((struct esp_hci_work *)work)->esp_hci_dev;

	gpiod_set_value(esp_hci_dev->pwr_gpio, 0);

	msleep(10);

	_state_change(esp_hci_dev, ESP_HCI_DEV_STATE_CLOSED);

	esp_hci_dev->next_rx_seq = 0;
}

static int esp_hci_open(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_info(esp_hci_dev->transport_dev, "HCI: opening...\n");

	struct esp_hci_work hci_work;
	INIT_WORK_ONSTACK(&hci_work.work, esp_hci_open_work);
	hci_work.esp_hci_dev = esp_hci_dev;

	queue_work(esp_hci_dev->wq, &hci_work.work);
	flush_work(&hci_work.work);

	int ret = wait_event_interruptible_timeout(
		esp_hci_dev->dev_state_change,
		esp_hci_dev->dev_state == ESP_HCI_DEV_STATE_OPEN,
		msecs_to_jiffies(5000));
	if (ret < 1) {
		dev_err(esp_hci_dev->transport_dev, "HCI: open failed\n");

		INIT_WORK_ONSTACK(&hci_work.work, esp_hci_close_work);
		queue_work(esp_hci_dev->wq, &hci_work.work);
		flush_work(&hci_work.work);

		return -ETIMEDOUT;
	}

	dev_info(esp_hci_dev->transport_dev, "HCI: opened!\n");

	return 0;
}

static int esp_hci_flush(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_info(esp_hci_dev->transport_dev, "HCI: flush\n");

	_flush_tx_queue(esp_hci_dev);
	flush_workqueue(esp_hci_dev->wq);

	return 0;
}

static int esp_hci_close(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_info(esp_hci_dev->transport_dev, "HCI: closing...\n");

	struct esp_hci_work close_work = {
		.esp_hci_dev = esp_hci_dev,
	};
	INIT_WORK_ONSTACK(&close_work.work, esp_hci_close_work);

	queue_work(esp_hci_dev->wq, &close_work.work);
	flush_work(&close_work.work);

	dev_info(esp_hci_dev->transport_dev, "HCI: closed!\n");

	return close_work.res;
}

static void esp_hci_reset(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_warn(esp_hci_dev->transport_dev, "HCI: reset not supported!\n");
}

static uint16_t compute_checksum(uint8_t *buf, uint16_t len)
{
	uint16_t checksum = 0;
	uint16_t i = 0;

	while (i < len) {
		checksum += buf[i];
		i++;
	}

	return checksum;
}

/* WARNING: @p skb should not be freed in case of error */
static int esp_hci_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);

	unsigned payload_len = skb->len;

	struct sk_buff *nskb;
	int header_len = esp_hci_dev->skb_expand(esp_hci_dev, skb, &nskb);
	if (header_len < 0) {
		dev_err_ratelimited(esp_hci_dev->transport_dev,
				    "HCI: skb_expand err: %d\n", header_len);
		return header_len;
	}

	BUG_ON(header_len + payload_len != nskb->len);

	struct esp_payload_header *hdr =
		(struct esp_payload_header *)nskb->data;
	*hdr = (struct esp_payload_header){
		.offset = cpu_to_le16(header_len),
		.if_type = ESP_IF_TYPE_HCI_HCI,
		.seq = 0, // will be added once enqueued
		.len = cpu_to_le16(payload_len),
		.hci_pkt_type = hci_skb_pkt_type(nskb),
	};

	hdr->checksum = cpu_to_le16(compute_checksum(nskb->data, nskb->len));
	// _debug_header(hdr, "TX");

	int ret = _send_packet(esp_hci_dev, nskb);
	if (nskb != skb) {
		/* SKB was reallocated:
		 *  - success: free the old SKB, we passed the new one to the
		 *    transport layer.
		 *  - failure: free the new SKB, the caller expects the old one
		 *    to be still alive. */
		kfree_skb(ret >= 0 ? skb : nskb);
	}

	return ret;
}

static int _fw_open(struct inode *inode, struct file *fp)
{
	struct esp_hci_dev *esp_hci_dev =
		container_of(inode->i_cdev, struct esp_hci_dev, fw_cdev);

	guard(mutex)(&esp_hci_dev->fw_dev_lock);

	if (esp_hci_dev->fw_dev_open) {
		return -EBUSY;
	}

	esp_hci_dev->fw_dev_open = true;

	return 0;
}

static int _fw_release(struct inode *inode, struct file *fp)
{
	struct esp_hci_dev *esp_hci_dev =
		container_of(inode->i_cdev, struct esp_hci_dev, fw_cdev);

	guard(mutex)(&esp_hci_dev->fw_dev_lock);

	/* Won't do anything in case no firmware download happened. */
	gpiod_set_value(esp_hci_dev->flash_gpio, 0);

	_register_hci_dev(esp_hci_dev);
	/* This has to happen after _register_hci_dev(). esp_hci_remove() might
	 * be waiting for this to call _unregister_hci_dev() -  we don't want
	 * those to race. */
	esp_hci_dev->fw_dev_open = false;
	wake_up_all(&esp_hci_dev->dev_state_change);

	return 0;
}

static ssize_t _fw_read(struct file *fp, char __user *buf, size_t count, loff_t *pos)
{
	struct esp_hci_dev *esp_hci_dev =
		container_of(fp->f_inode->i_cdev, struct esp_hci_dev, fw_cdev);

	guard(mutex)(&esp_hci_dev->fw_dev_lock);

	ssize_t res;
	if (*pos == 0) {
		guard(mutex)(&esp_hci_dev->fw_ver_lock);

		res = snprintf(
			esp_hci_dev->ver_str, sizeof(esp_hci_dev->ver_str),
			"{\"fw_ver\" : \"%u.%u.%u\", \"framing_ver\" : \"%u.%u.%u\"}",
			esp_hci_dev->fw_ver.major, esp_hci_dev->fw_ver.minor,
			esp_hci_dev->fw_ver.patch,
			esp_hci_dev->framing_ver.major,
			esp_hci_dev->framing_ver.minor,
			esp_hci_dev->framing_ver.patch);
		if (res <= 0) {
			return res;
		}

		esp_hci_dev->ver_str[sizeof(esp_hci_dev->ver_str) - 1] = 0;
	}

	ssize_t const data_available = strlen(esp_hci_dev->ver_str);

	if (*pos > data_available){
		return 0;
	}

	if (*pos + count >= data_available) {
		count = data_available - *pos;
	}

	if (copy_to_user(buf, esp_hci_dev->ver_str + *pos, count)) {
		return -EFAULT;
	}

	(*pos) += count;

	return count;
}

static ssize_t _fw_write(struct file *fp, char const __user *buf, size_t count, loff_t *pos)
{
	struct esp_hci_dev *esp_hci_dev =
		container_of(fp->f_inode->i_cdev, struct esp_hci_dev, fw_cdev);

	guard(mutex)(&esp_hci_dev->fw_dev_lock);

	if (esp_hci_dev->drv_state == ESP_HCI_DRV_STATE_FWUPD) {
		return count;
	}

	_unregister_hci_dev(esp_hci_dev);
	esp_hci_dev->drv_state = ESP_HCI_DRV_STATE_FWUPD;
	dev_info(esp_hci_dev->transport_dev,
		"esp_hci: entering FW download mode");

	if (esp_hci_dev->flash_gpio == NULL) {
		dev_warn(esp_hci_dev->transport_dev,
			"esp_hci: no flash mode pin assigned, you might need to set it manually!");
	} else {
		gpiod_set_value(esp_hci_dev->flash_gpio, 1);
	}

	_power_on_reset(esp_hci_dev);

	(void)buf;
	(void)pos;

	return count;
}

static int _create_fwdev(struct esp_hci_dev *esp_hci_dev)
{
	struct device *esp_dev = esp_hci_dev->transport_dev;

	guard(mutex)(&_fwdev_mgmt_lock);

	mutex_init(&esp_hci_dev->fw_dev_lock);

	int res;
	unsigned minor = 0;

	for (; minor < ESP_HCI_MINOR_CNT; minor++) {
		if (!__test_and_set_bit(minor, &_fwdev_map)) {
			break;
		}
	}

	if (minor >= ESP_HCI_MINOR_CNT) {
		dev_err(esp_dev, "esp_hci: reached dev cnt limit of %u\n",
			ESP_HCI_MINOR_CNT);
		return -ENODEV;
	}

	static const struct file_operations fw_cdev_fops = {
		.owner = THIS_MODULE,
		.open = _fw_open,
		.release = _fw_release,
		.read = _fw_read,
		.write = _fw_write,
	};
	cdev_init(&esp_hci_dev->fw_cdev, &fw_cdev_fops);

	res = cdev_add(&esp_hci_dev->fw_cdev, MKDEV(_fwdev_major, minor), 1);
	if (res < 0) {
		dev_err(esp_dev, "esp_hci: cdev_add() failed: %d\n", res);
		__clear_bit(minor, &_fwdev_map);
		return res;
	}

	esp_hci_dev->fw_device =
		device_create(_fwdev_class, esp_dev, MKDEV(_fwdev_major, minor), NULL,
			      "esp_hci_fw_%s",
			      esp_hci_dev->label ? esp_hci_dev->label : "none");
	if (IS_ERR(esp_hci_dev->fw_device)) {
		dev_err(esp_dev, "esp_hci: device_create() err: %ld\n",
			PTR_ERR(esp_hci_dev->fw_device));
			__clear_bit(minor, &_fwdev_map);
			cdev_del(&esp_hci_dev->fw_cdev);

		return PTR_ERR(esp_hci_dev->fw_device);
	}

	return 0;
}

static void _remove_fwdev(struct esp_hci_dev *esp_hci_dev)
{
	struct device *esp_dev = esp_hci_dev->transport_dev;

	mutex_lock(&_fwdev_mgmt_lock);

	if (!__test_and_clear_bit(MINOR(esp_hci_dev->fw_cdev.dev), &_fwdev_map)) {
		dev_err(esp_dev, "HCI: fwdev already unregistered");
	}

	mutex_unlock(&_fwdev_mgmt_lock);

	device_destroy(_fwdev_class, esp_hci_dev->fw_cdev.dev);
	cdev_del(&esp_hci_dev->fw_cdev);

	wait_event(esp_hci_dev->dev_state_change, !esp_hci_dev->fw_dev_open);
}

EXPORT_SYMBOL(esp_hci_probe);
int esp_hci_probe(struct esp_hci_dev *esp_hci_dev)
{
	struct device *dev = esp_hci_dev->transport_dev;

	skb_queue_head_init(&esp_hci_dev->tx_queue);
	init_waitqueue_head(&esp_hci_dev->dev_state_change);
	struct gpio_desc *rst_gpio = devm_gpiod_get(dev, "rst", GPIOD_OUT_LOW);
	if (IS_ERR(rst_gpio)) {
		dev_err(dev, "HCI: gpio init err: rst=%ld\n",
			PTR_ERR(rst_gpio));
		return -EINVAL;
	}
	esp_hci_dev->rst_gpio = rst_gpio;

	struct gpio_desc *pwr_gpio = devm_gpiod_get(dev, "pwr", GPIOD_OUT_LOW);
	if (IS_ERR(pwr_gpio)) {
		dev_warn(dev,
			 "HCI: no power pin provided, assuming always on.\n");
		esp_hci_dev->pwr_gpio = NULL;
	} else {
		esp_hci_dev->pwr_gpio = pwr_gpio;
	}

	struct gpio_desc *flash_gpio = devm_gpiod_get(dev, "flash", GPIOD_OUT_LOW);
	if (IS_ERR(flash_gpio)) {
		dev_warn(dev,
			 "HCI: no flash mode pin provided\n");
		esp_hci_dev->flash_gpio = NULL;
	} else {
		esp_hci_dev->flash_gpio = flash_gpio;
	}


	esp_hci_dev->wq = alloc_ordered_workqueue("esp_spi", 0);
	if (!esp_hci_dev->wq) {
		return -ENOMEM;
	}

	esp_hci_dev->dev_state = ESP_HCI_DEV_STATE_CLOSED;

	mutex_init(&esp_hci_dev->fw_ver_lock);

	int res = _register_hci_dev(esp_hci_dev);
	if (res < 0) {
		destroy_workqueue(esp_hci_dev->wq);
		return res;
	}

	res = of_property_read_string(dev->of_node, "label", &esp_hci_dev->label);
	if (res < 0) {
		dev_warn(dev, "HCI: device missing 'label' property in DT! (%d)", res);
	}

	res = _create_fwdev(esp_hci_dev);
	if (res < 0) {
		_unregister_hci_dev(esp_hci_dev);
		destroy_workqueue(esp_hci_dev->wq);
		return res;
	}

	return 0;
}

EXPORT_SYMBOL(esp_hci_remove);
void esp_hci_remove(struct esp_hci_dev *esp_hci_dev)
{
	/* This has to happen first because the fw device file make change the
	 * HCI device state. */
	_remove_fwdev(esp_hci_dev);
	_unregister_hci_dev(esp_hci_dev);
	/* Will sync the queue. */
	destroy_workqueue(esp_hci_dev->wq);
	gpiod_set_value(esp_hci_dev->pwr_gpio, 0);
	skb_queue_purge(&esp_hci_dev->tx_queue);
}

static int _check_framing_ver(struct esp_hci_dev *esp_hci_dev,
			  struct esp_hci_ver const *ver)
{
	if (ver->major != ESP_HCI_FRAMING_VER_MAJOR) {
		dev_err(esp_hci_dev->transport_dev,
			"HCI: framing ver incompatible: Linux: %d.%d.%d, ESP: %d.%d.%d\n",
			ESP_HCI_FRAMING_VER_MAJOR, ESP_HCI_FRAMING_VER_MINOR,
			ESP_HCI_FRAMING_VER_PATCH, ver->major, ver->minor,
			ver->patch);
		return -1;
	}

	if (ver->minor != ESP_HCI_FRAMING_VER_MINOR ||
	    ver->patch != ESP_HCI_FRAMING_VER_PATCH) {
		dev_warn(
			esp_hci_dev->transport_dev,
			"HCI: framing ver differ: Linux: %d.%d.%d, ESP: %d.%d.%d\n",
			ESP_HCI_FRAMING_VER_MAJOR, ESP_HCI_FRAMING_VER_MINOR,
			ESP_HCI_FRAMING_VER_PATCH, ver->major, ver->minor,
			ver->patch);
	}

	return 0;
}

static void update_fw_ver_info(struct esp_hci_dev *esp_hci_dev,
			       struct esp_hci_ver const *framing,
			       struct esp_hci_ver const *fw)
{
	guard(mutex)(&esp_hci_dev->fw_ver_lock);

	esp_hci_dev->framing_ver = *framing;
	esp_hci_dev->fw_ver = *fw;
}

static int process_event_esp_bootup(struct esp_hci_dev *esp_hci_dev,
				    struct sk_buff *skb)
{
	struct device *dev = esp_hci_dev->transport_dev;
	/* No need to register a reset if the dev wasn't on, e.g. on probe.
	 * Also, ignore it during fw update. */
	if (esp_hci_dev->dev_state == ESP_HCI_DEV_STATE_OPEN) {
		dev_warn_ratelimited(dev, "HCI: detected controller reset!\n");
		hci_reset_dev(esp_hci_dev->hci_dev);
		_state_change(esp_hci_dev, ESP_HCI_DEV_STATE_OPENING);
		return 0;
	}

	/* Drop any previous TX data. */
	_flush_tx_queue(esp_hci_dev);
	int res = 0;

	esp_hci_dev->caps = 0;

	struct esp_hci_ver framing_ver = { 0 };
	struct esp_hci_ver fw_ver = { 0 };

	while (skb->len >= sizeof(struct esp_cap_tag)) {
		struct esp_cap_tag *tag = (struct esp_cap_tag *)skb->data;

		if (skb->len < sizeof(struct esp_cap_tag) + tag->len) {
			dev_warn(dev, "HCI: esp cap tag overflow\n");
			res = -EINVAL;
			goto fail;
		}

		switch (tag->id) {
		case ESP_BOOTUP_CAPABILITY:
			if (tag->len < 1) {
				dev_warn(dev, "HCI: bootup cap tag invalid\n");
				res = -EINVAL;
				goto fail;
			}

			esp_hci_dev->caps = tag->data[0];
			break;
		case ESP_BOOTUP_FIRMWARE_CHIP_ID:
			dev_warn(dev, "HCI: skip chip id validation\n");
			break;
		case ESP_BOOTUP_FRAMING_VER:
		case ESP_BOOTUP_FW_VER:
			if (tag->len < sizeof(struct esp_hci_ver)) {
				dev_warn(dev,
					 "HCI: bootup ver data invalid\n");
				res = -EINVAL;
				break;
			}

			*(tag->id == ESP_BOOTUP_FRAMING_VER ?
				  &framing_ver :
				  &fw_ver) =
				*(struct esp_hci_ver const *)tag->data;

			break;
		case ESP_BOOTUP_SPI_CLK_MHZ:
			dev_warn(dev, "HCI: skip SPI clock reconfig\n");
			break;
		default:
			dev_warn(dev, "HCI: unsupported tag in bootup event (%d)\n",
				 tag->id);
		}

		skb_pull(skb, sizeof(struct esp_cap_tag) + tag->len);
	}

	update_fw_ver_info(esp_hci_dev, &framing_ver, &fw_ver);

	dev_info(dev, "HCI: FW ver = %d.%d.%d\n", esp_hci_dev->fw_ver.major,
		 esp_hci_dev->fw_ver.minor, esp_hci_dev->fw_ver.patch);

	if (_check_framing_ver(esp_hci_dev, &esp_hci_dev->framing_ver)) {
		res = -EINVAL;
		goto fail;
	}

	if (esp_hci_dev->caps & ESP_BT_SUPPORT) {
		dev_info(dev, "HCI: ESP supports HCI\n");

		_state_change(esp_hci_dev, ESP_HCI_DEV_STATE_OPEN);
	} else {
		dev_err(dev, "HCI: ESP does not support HCI\n");
		goto fail;
	}

	dev_dbg(dev, "HCI: caps = 0x%x\n", esp_hci_dev->caps);

	return 0;

fail:
	dev_err(dev, "HCI: ESP bootup failure\n");
	_state_change(esp_hci_dev, ESP_HCI_DEV_STATE_CLOSED);

	return res;
}

static void process_internal_event(struct esp_hci_dev *esp_hci_dev,
				   struct sk_buff *skb)
{
	struct device *dev = esp_hci_dev->transport_dev;

	struct event_header *header = (struct event_header *)skb->data;
	if (skb->len < header->len) {
		dev_warn(dev, "HCI: ESP event len overflow\n");
		return;
	}

	switch (header->event_code) {
	case ESP_INTERNAL_BOOTUP_EVENT:
	{
		struct esp_internal_bootup_event *evt =
			(struct esp_internal_bootup_event *)skb->data;
		if (skb->len < sizeof(struct esp_internal_bootup_event)) {
			dev_warn(dev, "HCI: ESP bootup event too small\n");
			break;
		}
		if (evt->header.status) {
			dev_warn(dev, "HCI: incorrect ESP bootup event\n");
			break;
		}

		skb_pull(skb, offsetof(struct esp_internal_bootup_event, data));

		if (skb->len < evt->len) {
			dev_warn(dev, "HCI: ESP bootup event len overflow\n");
			break;
		}

		skb_trim(skb, evt->len);

		process_event_esp_bootup(esp_hci_dev, skb);
		break;
	}
	default:
		dev_warn(dev, "HCI: %u unhandled internal event[%u]\n",
			 __LINE__, header->event_code);
		break;
	}
}

EXPORT_SYMBOL(esp_hci_rcv_pkt);
void esp_hci_rcv_pkt(struct esp_hci_dev *esp_hci_dev, struct sk_buff *_skb)
{
	struct sk_buff *skb __free(sk_buff) = _skb;
	struct hci_dev *hci_dev = esp_hci_dev->hci_dev;
	struct device *dev = esp_hci_dev->transport_dev;

	BUG_ON(skb->len < sizeof(struct esp_payload_header));

	// _debug_header((struct esp_payload_header *)skb->data, "RX");
	/* Make local copy. */
	struct esp_payload_header const hdr =
		*(struct esp_payload_header *)skb->data;

	if ((uint32_t)hdr.offset + hdr.len > skb->len) {
		dev_warn(dev, "HCI: %s: packet too big!\n", __func__);
		return;
	}

	if (hdr.len == 0) {
		return;
	}

	uint16_t rx_checksum = le16_to_cpu(hdr.checksum);
	((struct esp_payload_header *)skb->data)->checksum = 0;

	if (compute_checksum(skb->data, hdr.len + hdr.offset) != rx_checksum) {
		dev_warn_ratelimited(dev, "HCI: %s: checksum fail\n", __func__);
		return;
	}

	if (hdr.seq != esp_hci_dev->next_rx_seq) {
		dev_warn_ratelimited(dev,
				     "HCI: %s: RX seq = %u != %u expected\n",
				     __func__, hdr.seq,
				     esp_hci_dev->next_rx_seq);
	}
	esp_hci_dev->next_rx_seq = hdr.seq + 1;

	skb_pull(skb, hdr.offset);
	skb_trim(skb, hdr.len);

	switch ((enum ESP_INTERFACE_TYPE)hdr.if_type) {
	case ESP_INTERNAL_IF:
		if (esp_hci_dev->dev_state > ESP_HCI_DEV_STATE_CLOSED) {
			process_internal_event(esp_hci_dev, skb);
		}
		break;

	case ESP_HCI_IF:
		if (esp_hci_dev->dev_state < ESP_HCI_DEV_STATE_OPEN) {
			dev_warn_ratelimited(dev,
					     "HCI: %s: HCI packet for closed device!\n",
					     __func__);
			break;
		}
		if (skb->len < hdr.len) {
			dev_warn_ratelimited(dev,
					     "HCI: %s: HCI packet too big!\n",
					     __func__);
			break;
		}

		hci_skb_pkt_type(skb) = hdr.hci_pkt_type;

		/* Doesn't block, just queues. Takes over skb.*/
		int res = hci_recv_frame(hci_dev, no_free_ptr(skb));
		if (res < 0) {
			dev_err_ratelimited(dev,
					    "HCI: %s: hci_recv_frame err: %d\n",
					    __func__, res);
		}
		break;
	default:
		break;
	}
}

EXPORT_SYMBOL(esp_hci_pop_tx_packet);
struct sk_buff *esp_hci_pop_tx_packet(struct esp_hci_dev *esp_hci_dev)
{
	spin_lock(&esp_hci_dev->tx_queue.lock);

	struct sk_buff *tx_skb = __skb_dequeue(&esp_hci_dev->tx_queue);
	if (!tx_skb) {
		spin_unlock(&esp_hci_dev->tx_queue.lock);
		return NULL;
	}

	if (esp_hci_dev->tx_queue.qlen <= ESP_HCI_SPI_TX_QLEN_RESUME) {
		esp_hci_dev->tx_paused = false;
	}

	spin_unlock(&esp_hci_dev->tx_queue.lock);

	return tx_skb;
}

static void _flush_tx_queue(struct esp_hci_dev *esp_hci_dev)
{
	spin_lock(&esp_hci_dev->tx_queue.lock);
	__skb_queue_purge(&esp_hci_dev->tx_queue);
	esp_hci_dev->next_tx_seq = 0;
	spin_unlock(&esp_hci_dev->tx_queue.lock);
}

static int _send_packet(struct esp_hci_dev *esp_hci_dev, struct sk_buff *skb)
{
	spin_lock(&esp_hci_dev->tx_queue.lock);

	if (esp_hci_dev->tx_paused) {
		spin_unlock(&esp_hci_dev->tx_queue.lock);
		return -EBUSY;
	}

	size_t queue_len = esp_hci_dev->tx_queue.qlen;
	if (queue_len >= ESP_HCI_SPI_TX_QLEN_MAX) {
		esp_hci_dev->tx_paused = true;
		spin_unlock(&esp_hci_dev->tx_queue.lock);

		dev_warn_ratelimited(esp_hci_dev->transport_dev,
				     "TX queue limit reached!\n");
		return -EBUSY;
	}

	struct esp_payload_header *header = (struct esp_payload_header *)skb->data;
	header->seq = esp_hci_dev->next_tx_seq++;
	header->checksum += header->seq;

	__skb_queue_tail(&esp_hci_dev->tx_queue, skb);

	spin_unlock(&esp_hci_dev->tx_queue.lock);

	if (queue_len == 0) {
		esp_hci_dev->tx_ready(esp_hci_dev);
	}

	return 0;
}

static int __init esp_hci_init(void)
{
	printk("esp_hci: init\n");
	dev_t fwdev;
	int res = alloc_chrdev_region(&fwdev, 0, ESP_HCI_MINOR_CNT, "esp_hci");
	if (res < 0) {
		printk("esp_hci: alloc_chrdev_region() failed: %d\n", res);
		return res;
	}
	_fwdev_major = MAJOR(fwdev);

	_fwdev_class = class_create("fw_update");
	if (IS_ERR(_fwdev_class)) {
		printk("esp_hci: class_create() failed: %ld\n", PTR_ERR(_fwdev_class));
		unregister_chrdev_region(MKDEV(_fwdev_major, 0), ESP_HCI_MINOR_CNT);
		return PTR_ERR(_fwdev_class);
	}

	return 0;
}

static void __exit esp_hci_exit(void)
{
	printk("esp_hci: exit\n");
	class_destroy(_fwdev_class);
	unregister_chrdev_region(MKDEV(_fwdev_major, 0), ESP_HCI_MINOR_CNT);
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mihai Renea <mihai.renea@ml-pa.com>");
MODULE_DESCRIPTION("HCI driver for ESP-Hosted");
// MODULE_VERSION(RELEASE_VERSION);
module_init(esp_hci_init);
module_exit(esp_hci_exit);