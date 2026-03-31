/* SPDX-License-Identifier: GPL-2.0-only
 *
 * This implementation is derived from the ESP hosted project available at
 * https://github.com/espressif/esp-hosted which is
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 */
#pragma once

#include "linux/cdev.h"
#include <linux/device.h>
#include <linux/skbuff.h>
#include <linux/wait.h>

#include <net/bluetooth/bluetooth.h> // has to go before hci_core.h for reasons
#include <net/bluetooth/hci_core.h>

struct esp_hci_work {
	struct work_struct work;
	struct esp_hci_dev *esp_hci_dev;
	int res;
};

struct esp_hci_ver {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
} __packed;

/**
 * struct esp_hci_state_t - ESP HCI device state
 *
 * @ESP_HCI_STATE_CLOSED: device is powered off or in an unknown state
 * @ESP_HCI_STATE_OPENING: power on/reset sequence triggered.
 * @ESP_HCI_STATE_OPEN: the device on and is functional.
 * @ESP_HCI_STATE_FWUPD: the device is on and functional, but is performing a
 *			 firmware update. The device cannot be powered off/reset
 *			 while in this state.
 *
 * ** DO NOT REORDER! **
 */
typedef enum {
	ESP_HCI_STATE_CLOSED = 0,
	ESP_HCI_STATE_OPENING = 1,
	ESP_HCI_STATE_OPEN = 2,
	ESP_HCI_STATE_FWUPD = 3,
} esp_hci_state_t;

/**
 * struct esp_hci_dev - ESP HCI device structure
 *
 * Represents a generic ESP HCI device and is expanded by the transport layer.
 *
 * @type: Transport layer type. Search for "HCI bus types" is hci.h.
 * @transport_dev: Underlying kernel device associated with the transport.
 * @pwr_gpio: Controller reset.
 * @pwr_gpio: Controller power supply control. May be NULL if missing.
 * @caps: Capabilities flags.
 * @hci_dev: Kernel HCI core device.
 * @wq: Workqueue for serializing any state change.
 * @state: device state
 * @state_change: signals when the device state changes
 * @label: 'label' property from device tree, NULL if missing.
 * @fw_cdev: firmware character device
 * @fw_device: firmware device
 * @fw_dev_lock: serializes fw device file operations
 * @fw_ver: firmware version, set when device boots
 * @framing_ver: transport framing version, set when device boots
 * @ver_str: storage for the version string, read by the firmware character dev
 * @next_tx_seq: seq no of the next frame going out.
 * @next_rx_seq: expected seq no of the next frame coming in.
 */
struct esp_hci_dev {
	/* The following fields are set up by the transport layer before calling
	 * esp_hci_probe(): */

	__u8 type;
	/**
	 * @write_packet: Send HCI packet to the controller.
	 * SHALL NOT free @skb on error. Purges TX queue if @skb == NULL.
	 *
	 * Returns 0 on success, negative error otherwise.
	 */
	int (*write_packet)(struct esp_hci_dev *esp_hci_dev,
			    struct sk_buff *skb);
	/**
	 * @write_packet: Expand TX skb as required by the transport layer..
	 *
	 * Expands or reallocs @skb headroom and tailroom. On success, *@nskbp
	 * points to the expanded skb (either original or newly allocated).
	 * @skb is NOT freed in either case.
	 *
	 * *@nskbp->data is pushed back by the allocated headroom, and the
	 * headroom value (payload offset) is returned.
	 *
	 * Returns payload offset on success, negative error otherwise.
	 */
	int (*skb_expand)(struct esp_hci_dev *esp_hci_dev, struct sk_buff *skb,
			  struct sk_buff **nskbp);
	struct device *transport_dev;

	/* The following fields are set by the ESP HCI generic implementation. */

	struct gpio_desc *rst_gpio;
	struct gpio_desc *pwr_gpio;
	unsigned caps;
	struct hci_dev *hci_dev;
	struct workqueue_struct *wq;
	esp_hci_state_t state;
	struct wait_queue_head state_change;
	char const *label;

	struct cdev fw_cdev;
	struct device *fw_device;
	struct mutex fw_dev_lock;

	struct esp_hci_ver fw_ver;
	struct esp_hci_ver framing_ver;

	char ver_str[80];

	/* Used to track transport layer frame losses. */

	uint8_t next_tx_seq;
	uint8_t next_rx_seq;
};

/**
 * struct esp_payload_header: Header for framing between controller and host
 *
 * @if_type: interface type (HCI or internal). Set to 0x0F for empty frame.
 * @seq: for detecting frame losses.
 * @reserved: reserved
 * @len: payload len (excluding header + padding)
 * @offset: where payload starts relative to header begin (header size + padding)
 * @checksum: simple checksum over the whole frame, with this field set to 0
 * @reserved2: reserved
 * @hci_pkt_type: HCI packet type, provided separately by the HCI layer.
 */
struct esp_payload_header {
	uint8_t if_type;
	uint8_t seq;
	uint32_t reserved;
	uint16_t len;
	uint16_t offset;
	uint16_t checksum;
	union {
		uint8_t reserved2;
		uint8_t hci_pkt_type; /* Packet type for HCI interface */
	};
	/* Do no add anything here */
} __packed;

/**
 * esp_hci_probe - Probe ESP HCI device.
 *
 * Called by the transport layer to set up the common HCI stuff.
 *
 * @esp_hci_dev: allocated and initialized ESP HCI device. See &struct
 *               esp_hci_dev for which part of the struct to init.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int esp_hci_probe(struct esp_hci_dev *esp_hci_dev);

/**
 * esp_hci_remove - Remove ESP HCI device.
 *
 * Called by the transport layer to remove the common HCI stuff.
 *
 * SHALL NOT be called from the &esp_hci_dev->wq or it will deadlock.
 *
 * @esp_hci_dev: previously init with esp_hci_probe()
 *
 * Return: 0 on success, negative error code otherwise.
 */
void esp_hci_remove(struct esp_hci_dev *esp_hci_dev);

/**
 * esp_hci_rcv_pkt - Forward a received HCI packet to the ESP HCI layer.
 *
 * MUST be called from &esp_hci_dev->wq otherwise it will race against
 * device state changes.
 *
 * @esp_hci_dev: ESP HCI dev
 * @pkt: received frame. The SKB length MUST be at least sizeof(&struct
 *       esp_payload_header) bytes.
 */
void esp_hci_rcv_pkt(struct esp_hci_dev *esp_hci_dev, struct sk_buff *pkt);

DEFINE_FREE(sk_buff, struct sk_buff *, if (_T) kfree_skb(_T));
