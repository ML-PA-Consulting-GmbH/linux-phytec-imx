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
 * work queues.
 */
#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "esp_hci.h"

#define ESP_IF_TYPE_HCI_HCI 2

#define ESP_HCI_API_VER_MAJOR 2
#define ESP_HCI_API_VER_MINOR 0
#define ESP_HCI_API_VER_PATCH 1

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
	ESP_BOOTUP_FW_DATA,
	ESP_BOOTUP_SPI_CLK_MHZ,
	ESP_BOOTUP_FIRMWARE_CHIP_ID,
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

struct esp_hci_api_ver {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
} __packed;

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

static int _register_hci_dev(struct esp_hci_dev *esp_hci_dev)
{
	int res = hci_register_dev(esp_hci_dev->hci_dev);
	if (res < 0) {
		dev_err(esp_hci_dev->transport_dev,
			"HCI: cannot register HCI dev: %d\n", res);
		return res;
	}

	dev_info(esp_hci_dev->transport_dev, "HCI: registered HCI dev\n");

	return 0;
}

static void _unregister_hci_dev(struct esp_hci_dev *esp_hci_dev)
{
	/* Oops if the HCI dev was already unregistered. */
	hci_unregister_dev(esp_hci_dev->hci_dev);
	dev_info(esp_hci_dev->transport_dev, "HCI: unregistered HCI dev\n");
}

static int esp_hci_open(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_info(esp_hci_dev->transport_dev, "HCI: open\n");

	flush_work(&esp_hci_dev->close_work.work);

	/* If power pin is not supported, trigger a reset to put the controller
	 * in a clean state. Otherwise this won't do anything, as the dev should
	 * be off.*/
	gpiod_set_value(esp_hci_dev->rst_gpio, 1);
	msleep(3);
	gpiod_set_value(esp_hci_dev->rst_gpio, 0);

	gpiod_set_value(esp_hci_dev->pwr_gpio, 1);

	int ret = wait_event_interruptible_timeout(esp_hci_dev->wait_open,
						   esp_hci_dev->is_open,
						   msecs_to_jiffies(5000));
	if (ret < 1) {
		dev_err(esp_hci_dev->transport_dev, "HCI: open failed\n");
		return -ETIMEDOUT;
	}

	dev_info(esp_hci_dev->transport_dev, "HCI: opened\n");

	return 0;
}

static int esp_hci_flush(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_info(esp_hci_dev->transport_dev, "HCI: flush\n");

	esp_hci_dev->write_packet(esp_hci_dev, NULL);

	return 0;
}

static void esp_hci_close_work(struct work_struct *work)
{
	struct esp_hci_dev *esp_hci_dev = ((struct esp_hci_work *)work)->esp_hci_dev;
	gpiod_set_value(esp_hci_dev->pwr_gpio, 0);

	msleep(10);

	esp_hci_dev->is_open = false;
	esp_hci_dev->next_rx_seq = 0;
	esp_hci_dev->next_tx_seq = 0;
}

static int esp_hci_close(struct hci_dev *hdev)
{
	struct esp_hci_dev *esp_hci_dev = hci_get_drvdata(hdev);
	dev_info(esp_hci_dev->transport_dev, "HCI: close\n");

	queue_work(esp_hci_dev->wq, &esp_hci_dev->close_work.work);

	return 0;
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
		.seq = esp_hci_dev->next_tx_seq++,
		.len = cpu_to_le16(payload_len),
		.hci_pkt_type = hci_skb_pkt_type(nskb),
	};

	hdr->checksum = cpu_to_le16(compute_checksum(nskb->data, nskb->len));
	// _debug_header(hdr, "TX");

	int ret = esp_hci_dev->write_packet(esp_hci_dev, nskb);

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

EXPORT_SYMBOL(esp_hci_probe);
int esp_hci_probe(struct esp_hci_dev *esp_hci_dev)
{
	struct device *dev = esp_hci_dev->transport_dev;

	init_waitqueue_head(&esp_hci_dev->wait_open);
	struct gpio_desc *rst_gpio = devm_gpiod_get(dev, "rst", GPIOD_OUT_LOW);
	if (IS_ERR(rst_gpio)) {
		dev_err(dev, "HCI: gpio init err: rst=%ld\n",
			PTR_ERR(rst_gpio));
		return -EINVAL;
	}
	esp_hci_dev->rst_gpio = rst_gpio;

	INIT_WORK(&esp_hci_dev->close_work.work, esp_hci_close_work);
	esp_hci_dev->close_work.esp_hci_dev = esp_hci_dev;

	struct gpio_desc *pwr_gpio = devm_gpiod_get(dev, "pwr", GPIOD_OUT_HIGH);
	if (IS_ERR(pwr_gpio)) {
		dev_warn(dev,
			 "HCI: no power pin provided, assuming always on.\n");
		esp_hci_dev->pwr_gpio = NULL;
	} else {
		esp_hci_dev->pwr_gpio = pwr_gpio;
	}

	esp_hci_dev->wq = alloc_ordered_workqueue("esp_spi", 0);
	if (!esp_hci_dev->wq) {
		return -ENOMEM;
	}

	struct hci_dev *hci_dev = hci_alloc_dev();
	if (!hci_dev) {
		dev_err(dev, "HCI: hci_alloc_dev err\n");
		destroy_workqueue(esp_hci_dev->wq);
		return -ENOMEM;
	}

	hci_set_drvdata(hci_dev, esp_hci_dev);
	esp_hci_dev->hci_dev = hci_dev;

	hci_dev->bus = esp_hci_dev->type;
	hci_dev->close = esp_hci_close;
	hci_dev->open = esp_hci_open;
	hci_dev->send = esp_hci_send;
	hci_dev->reset = esp_hci_reset;
	hci_dev->flush = esp_hci_flush;

	int res = _register_hci_dev(esp_hci_dev);
	if (res < 0) {
		hci_free_dev(hci_dev);
		destroy_workqueue(esp_hci_dev->wq);
		return res;
	}

	return 0;
}

struct esp_hci_remove_work {
	struct work_struct work;
	struct esp_hci_dev *esp_hci_dev;
};

static void _unregister_work_fn(struct work_struct *work)
{
	struct esp_hci_dev *esp_hci_dev =
		((struct esp_hci_work *)work)->esp_hci_dev;
	esp_hci_dev->is_open = false;
	/* Will sync the HCI work queues, including any pending tx work. */
	_unregister_hci_dev(esp_hci_dev);
}

EXPORT_SYMBOL(esp_hci_remove);
void esp_hci_remove(struct esp_hci_dev *esp_hci_dev)
{
	struct esp_hci_work unreg_work;
	INIT_WORK_ONSTACK(&unreg_work.work, _unregister_work_fn);
	unreg_work.esp_hci_dev = esp_hci_dev;

	queue_work(esp_hci_dev->wq, &unreg_work.work);
	/* will sync any pending work */
	destroy_workqueue(esp_hci_dev->wq);

	hci_free_dev(esp_hci_dev->hci_dev);
	esp_hci_dev->hci_dev = NULL;

	if (esp_hci_dev->pwr_gpio) {
		gpiod_set_value(esp_hci_dev->pwr_gpio, 0);
	}
}

static int _check_api_ver(struct esp_hci_dev *esp_hci_dev,
			  struct esp_hci_api_ver const *ver)
{
	if (ver->major != ESP_HCI_API_VER_MAJOR) {
		dev_err(esp_hci_dev->transport_dev,
			"HCI: API ver incompatible: Linux: %d.%d.%d, ESP: %d.%d.%d\n",
			ESP_HCI_API_VER_MAJOR, ESP_HCI_API_VER_MINOR,
			ESP_HCI_API_VER_PATCH, ver->major, ver->minor,
			ver->patch);
		return -1;
	}

	if (ver->minor != ESP_HCI_API_VER_MINOR ||
	    ver->patch != ESP_HCI_API_VER_PATCH) {
		dev_warn(
			esp_hci_dev->transport_dev,
			"HCI: API ver differ: Linux: %d.%d.%d, ESP: %d.%d.%d\n",
			ESP_HCI_API_VER_MAJOR, ESP_HCI_API_VER_MINOR,
			ESP_HCI_API_VER_PATCH, ver->major, ver->minor,
			ver->patch);
	}

	return 0;
}

static int process_event_esp_bootup(struct esp_hci_dev *esp_hci_dev,
				    struct sk_buff *skb)
{
	struct device *dev = esp_hci_dev->transport_dev;
	/* No need to register a reset if the dev wasn't on, e.g. on probe. */
	if (hdev_is_powered(esp_hci_dev->hci_dev)) {
		dev_warn_ratelimited(dev, "HCI: detected controller reset!\n");
		hci_reset_dev(esp_hci_dev->hci_dev);
	}

	/* Drop any previous TX data. */
	esp_hci_dev->write_packet(esp_hci_dev, NULL);
	int res = 0;

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
		case ESP_BOOTUP_FW_DATA:
			if (tag->len < sizeof(struct esp_hci_api_ver)) {
				dev_warn(dev,
					 "HCI: bootup API ver data invalid\n");
				res = -EINVAL;
				goto fail;
			}
			if (_check_api_ver(esp_hci_dev,
					   (struct esp_hci_api_ver const *)
						   tag->data)) {
				res = -EINVAL;
				goto fail;
			}
			break;
		case ESP_BOOTUP_SPI_CLK_MHZ:
			dev_warn(dev, "HCI: skip SPI clock reconfig\n");
			break;
		default:
			dev_warn(dev, "HCI: unsupported tag in bootup event\n");
		}

		skb_pull(skb, sizeof(struct esp_cap_tag) + tag->len);
	}

	if (esp_hci_dev->caps & ESP_BT_SUPPORT) {
		dev_info(dev, "HCI: ESP supports HCI\n");

		esp_hci_dev->is_open = true;
		wake_up(&esp_hci_dev->wait_open);
	} else {
		dev_err(dev, "HCI: ESP does not support HCI\n");
		goto fail;
	}

	dev_dbg(dev, "HCI: caps = 0x%x\n", esp_hci_dev->caps);

	return 0;

fail:
	dev_err(dev, "HCI: ESP bootup failure\n");
	esp_hci_dev->is_open = false;

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
		process_internal_event(esp_hci_dev, skb);
		break;

	case ESP_HCI_IF:
		if (!esp_hci_dev->is_open) {
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

static int __init esp_hci_init(void)
{
	printk("esp_hci: init\n");
	return 0;
}

static void __exit esp_hci_exit(void)
{
	printk("esp_hci: exit\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mihai Renea <mihai.renea@ml-pa.com>");
MODULE_DESCRIPTION("HCI driver for ESP-Hosted");
// MODULE_VERSION(RELEASE_VERSION);
module_init(esp_hci_init);
module_exit(esp_hci_exit);
