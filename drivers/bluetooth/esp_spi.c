/* SPDX-License-Identifier: GPL-2.0-only
 *
 * This implementation is derived from the ESP hosted project available at
 * https://github.com/espressif/esp-hosted which is
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 */
#include <linux/module.h>
#include <linux/spi/spidev.h>
#include <linux/workqueue.h>
#include <linux/align.h>

#include "esp_hci.h"

#define ESP_HCI_SPI_FRAME_SIZE 300
#define ESP_HCI_SPI_ALIGN sizeof(unsigned)
#define ESP_HCI_SPI_XFER_LIM 16

struct esp_hci_spi_dev {
	struct esp_hci_dev esp_hci_dev;
	struct gpio_desc *hs_gpio;
	struct gpio_desc *dr_gpio;
	int hs_irq;
	int dr_irq;
	struct delayed_work transfer_dwork;
	unsigned char *tx_empty_frame;
};


/* return < 0 on err, 0 if there's nothing to transfer, > 0  otherwise */
static int _transfer_once(struct esp_hci_spi_dev *hci_spi_dev,
			  struct spi_device *spi_dev)
{
	int trans_ready = gpiod_get_value(hci_spi_dev->hs_gpio);
	int rx_pending = gpiod_get_value(hci_spi_dev->dr_gpio);
	BUG_ON(trans_ready < 0);
	BUG_ON(rx_pending < 0);

	if (!trans_ready) {
		/* We'll come back once the interrupt triggers */
		return 0;
	}

	struct sk_buff *tx_skb __free(sk_buff) =
		esp_hci_pop_tx_packet(&hci_spi_dev->esp_hci_dev);
	if (!rx_pending && !tx_skb) {
		/* We'll come back once the RX interrupt triggers, or
		 * the first packet in the empty TX queue is pushed. */
		return 0;
	}
	/* There's a race between checking the DR pin and starting the
	 * SPI transfer. We may see the DR pin low, then it might go
	 * high while we're starting the transfer. In that case we would
	 * discard valid data. Instead, just forward any transfer to the
	 * upper layer and let it decide if it's valid or not.
	 *
	 * TODO: optimization: re-purpose the old TX skb for RX */
	struct sk_buff *rx_skb __free(sk_buff) = alloc_skb(
		ESP_HCI_SPI_FRAME_SIZE + ESP_HCI_SPI_ALIGN - 1, GFP_KERNEL);
	if (!rx_skb) {
		return -ENOMEM;
	}

	unsigned char *const data_a =
		PTR_ALIGN(rx_skb->data, ESP_HCI_SPI_ALIGN);
	BUG_ON(data_a - rx_skb->data > ESP_HCI_SPI_ALIGN - 1);
	BUG_ON((uintptr_t)data_a & (ESP_HCI_SPI_ALIGN - 1));

	skb_reserve(rx_skb, (int)(data_a - rx_skb->data));
	memset(rx_skb->data, 0, ESP_HCI_SPI_FRAME_SIZE);

	struct spi_transfer transfer = {
		.tx_buf = tx_skb ? tx_skb->data : hci_spi_dev->tx_empty_frame,
		.rx_buf = rx_skb->data,
		.len = ESP_HCI_SPI_FRAME_SIZE,
	};
	skb_put(rx_skb, ESP_HCI_SPI_FRAME_SIZE);

	int res = spi_sync_transfer(spi_dev, &transfer, 1);
	if (res < 0) {
		return res;
	}

	esp_hci_rcv_pkt(&hci_spi_dev->esp_hci_dev, no_free_ptr(rx_skb));

	return 1;
}

static void _transfer_work(struct work_struct *work)
{
	struct esp_hci_spi_dev *hci_spi_dev =
		container_of(work, struct esp_hci_spi_dev, transfer_dwork.work);
	struct spi_device *spi_dev =
		(struct spi_device *)hci_spi_dev->esp_hci_dev.transport_dev;

	for (unsigned limit = ESP_HCI_SPI_XFER_LIM; limit; limit--) {
		int res = _transfer_once(hci_spi_dev, spi_dev);
		if (res <= 0) {
			if (res < 0) {
				dev_err_ratelimited(&spi_dev->dev,
						    "transfer err: %d", res);
				queue_delayed_work(hci_spi_dev->esp_hci_dev.wq,
						   &hci_spi_dev->transfer_dwork,
						   msecs_to_jiffies(1000));
			}
			return;
		}
	}
	/* The interrupts are enabled the whole time. Any activity on the HS line
	 * during the execution of this work will re-queue it. This work item
	 * ends here because either:
	 *  1. We reached the ESP_HCI_SPI_XFER_LIM -> HS toggled -> queued by HS irq
	 *  2. HS is not set -> queued by future HS irq
	 *  3. Error condition -> queued here with a delay */
}

static int _skb_expand(struct esp_hci_dev *esp_hci_dev, struct sk_buff *skb,
		       struct sk_buff **nskbp)
{
	size_t const max_header =
		sizeof(struct esp_payload_header) + ESP_HCI_SPI_ALIGN - 1;
	size_t const max_frame = max_header + skb->len;
	if (max_frame > ESP_HCI_SPI_FRAME_SIZE) {
		dev_err_ratelimited(
			esp_hci_dev->transport_dev,
			"total payload = %lu > %u = max SPI frame size\n",
			max_frame, ESP_HCI_SPI_FRAME_SIZE);
		return -ENOSPC;
	}

	/* Min tailroom required s.t. the whole allocation is at least the
	 * frame size. */
	size_t const min_tailroom = ESP_HCI_SPI_FRAME_SIZE -
				    sizeof(struct esp_payload_header) -
				    skb->len;
	/* SKB allocations are always word-aligned. If there's enough headroom
	 * to fit our header without padding, then we know that header can be
	 * word-aligned. */
	if (unlikely(skb_shared(skb))) {
		skb = skb_copy_expand(skb, sizeof(struct esp_payload_header),
				      min_tailroom, GFP_KERNEL);
		if (!skb) {
			return -ENOMEM;
		}
	} else {
		int res = pskb_expand_head(skb,
					   sizeof(struct esp_payload_header),
					   min_tailroom, GFP_KERNEL);
		if (res < 0) {
			return -ENOMEM;
		}
	}

	*nskbp = skb;

	unsigned char *const data_a =
		PTR_ALIGN_DOWN(skb->data - sizeof(struct esp_payload_header),
			       ESP_HCI_SPI_ALIGN);
	unsigned const headroom = skb->data - data_a;

	BUG_ON(headroom > max_header);
	BUG_ON(headroom > skb_headroom(skb));

	__skb_push(skb, headroom);

	BUG_ON(skb->data != data_a);
	BUG_ON(skb->len + skb_tailroom(skb) < ESP_HCI_SPI_FRAME_SIZE);

	/* Technically not needed, but shoving garbage down the bus makes
	 * debugging harder and may expose kernel memory. */
	memset(skb->data, 0, headroom);
	memset(skb_tail_pointer(skb), 0, skb_tailroom(skb));

	return headroom;
}

static const struct of_device_id _of_match_table[] = {
	{ .compatible = "esp,hosted" },
	{},
};
MODULE_DEVICE_TABLE(of, _of_match_table);

static struct spi_device_id const _id_table[] = {
	{ .name = "hosted" },
	{},
};
MODULE_DEVICE_TABLE(spi, _id_table);

static irqreturn_t _start_xfer_handler(int irq, void *priv)
{
	struct esp_hci_spi_dev *hci_spi_dev = priv;
	queue_work(hci_spi_dev->esp_hci_dev.wq,
		   &hci_spi_dev->transfer_dwork.work);

	return IRQ_HANDLED;
}

__alias(_start_xfer_handler) static irqreturn_t
	_dr_handler(int irq, void *priv);
__alias(_start_xfer_handler) static irqreturn_t
	_hs_handler(int irq, void *priv);

static int _init_gpios(struct esp_hci_spi_dev *hci_spi_dev)
{
	struct device *dev = hci_spi_dev->esp_hci_dev.transport_dev;

	struct gpio_desc *hs_gpio, *dr_gpio = NULL;
	hs_gpio = devm_gpiod_get(dev, "hs", GPIOD_IN);
	dr_gpio = devm_gpiod_get(dev, "dr", GPIOD_IN);
	if (IS_ERR(hs_gpio) || IS_ERR(dr_gpio)) {
		dev_err(dev, "gpio init err: hs=%ld, dr=%ld\n",
			PTR_ERR(hs_gpio), PTR_ERR(dr_gpio));

		return -1;
	}

	int hs_irq = gpiod_to_irq(hs_gpio);
	int dr_irq = gpiod_to_irq(dr_gpio);
	if (hs_irq < 0 || dr_irq < 0) {
		dev_err(dev, "failed to obtain interrupts: hs=%d, dr=%d\n",
			hs_irq, dr_irq);
		return -1;
	}

	int res = devm_request_irq(dev, hs_irq, _hs_handler,
				   IRQF_TRIGGER_RISING, "esp_hs", hci_spi_dev);
	if (res < 0) {
		dev_err(dev, "failed to request hs_irq\n");
		return res;
	}

	res = devm_request_irq(dev, dr_irq, _dr_handler, IRQF_TRIGGER_RISING,
			       "esp_dr", hci_spi_dev);
	if (res < 0) {
		dev_err(dev, "failed to request hs_irq\n");
		return res;
	}

	hci_spi_dev->dr_gpio = dr_gpio;
	hci_spi_dev->hs_gpio = hs_gpio;
	hci_spi_dev->dr_irq = dr_irq;
	hci_spi_dev->hs_irq = hs_irq;

	return 0;
}

static void _tx_ready(struct esp_hci_dev *esp_hci_dev)
{
	struct esp_hci_spi_dev *esp_spi_dev =
		container_of(esp_hci_dev, struct esp_hci_spi_dev, esp_hci_dev);

	queue_work(esp_spi_dev->esp_hci_dev.wq,
		   &esp_spi_dev->transfer_dwork.work);
}

static int _probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;

	spi->mode = SPI_MODE_2;
	// spi->max_speed_hz = 5000000;
	dev_info(dev, "SPI at %u Hz\n", spi->max_speed_hz);

	int res = spi_setup(spi);
	if (res < 0) {
		dev_err(dev, "SPI setup failed!\n");
		return res;
	}

	struct esp_hci_spi_dev *hci_spi_dev = devm_kzalloc(
		&spi->dev, sizeof(struct esp_hci_spi_dev), GFP_KERNEL);
	if (!hci_spi_dev) {
		return -ENOMEM;
	}

	dev_set_drvdata(&spi->dev, hci_spi_dev);

	*hci_spi_dev = (struct esp_hci_spi_dev){
		.esp_hci_dev.type = HCI_SPI,
		.esp_hci_dev.tx_ready = _tx_ready,
		.esp_hci_dev.skb_expand = _skb_expand,
		.esp_hci_dev.transport_dev = &spi->dev,
		.tx_empty_frame = devm_kzalloc(
			&spi->dev, ESP_HCI_SPI_FRAME_SIZE, GFP_KERNEL),
	};

	if (!hci_spi_dev->tx_empty_frame) {
		return -ENOMEM;
	}

	((struct esp_payload_header *)hci_spi_dev->tx_empty_frame)->if_type =
		0x0F;
	INIT_DELAYED_WORK(&hci_spi_dev->transfer_dwork, _transfer_work);
	/* _init_gpios() below will enable the interrupts but we're not ready
	 * yet. */
	disable_delayed_work(&hci_spi_dev->transfer_dwork);

	res = _init_gpios(hci_spi_dev);
	if (res < 0) {
		dev_err(dev, "failed to init gpios: %d\n", res);
		return res;
	}

	res = esp_hci_probe(&hci_spi_dev->esp_hci_dev);
	if (res < 0) {
		dev_err(dev, "esp_hci_probe failed: %d\n", res);
		return res;
	}

	enable_and_queue_work(hci_spi_dev->esp_hci_dev.wq,
			      &hci_spi_dev->transfer_dwork.work);

	dev_dbg(dev, "probed!\n");
	return 0;
}

static void _remove(struct spi_device *spi)
{
	struct esp_hci_spi_dev *hci_spi_dev = dev_get_drvdata(&spi->dev);
	/* Make sure our transfer work item is finished and won't get queued
	 * again. */
	disable_delayed_work_sync(&hci_spi_dev->transfer_dwork);

	/* This has to happen after cancelling the work item since it may
	 * re-enable them. We need to do this since we will destroy the work
	 * queue. The IRQs would otherwise get disabled only when the SPI device
	 * itself will be removed completely.
	 *
	 * WARNING: nesting calls! */
	disable_irq(hci_spi_dev->dr_irq);
	disable_irq(hci_spi_dev->hs_irq);

	/* This will call hci_unregister_dev(), which will cancel and sync with
         * the HCI core TX work item. After this returns, we know for sure that
	 * the upper layer is done with us, in particular that _write_packet()
	 * is finished. */
	esp_hci_remove(&hci_spi_dev->esp_hci_dev);

	dev_dbg(&spi->dev, "removed!\n");
}

static struct spi_driver _driver = {
	.id_table = _id_table,
	.probe = _probe,
	.remove = _remove,
	.shutdown = NULL,
	.driver = {
		.name = "esp_spi",
		.of_match_table = _of_match_table,
	},
};

static int __init esp_spi_init(void)
{
	int ret = spi_register_driver(&_driver);
	if (ret < 0) {
		printk("esp_spi: spi_register_driver() failed: %d\n", ret);
		return ret;
	}

	printk("esp_spi: loaded\n");
	return 0;
}
module_init(esp_spi_init);

static void __exit esp_spi_exit(void)
{
	printk("esp_spi: exit\n");
	spi_unregister_driver(&_driver);
}
module_exit(esp_spi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mihai Renea <mihai.renea@ml-pa.com>");
MODULE_DESCRIPTION("SPI transport for ESP-Hosted");
// MODULE_VERSION(RELEASE_VERSION);
