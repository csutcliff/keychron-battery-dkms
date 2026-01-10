// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID driver for Keychron mouse battery reporting
 *
 * Copyright (c) 2026 Chris Sutcliff <chris@sutcliff.me>
 *
 * This driver queries battery level from Keychron wireless mice via
 * vendor-specific HID commands. It sends a status request on the control
 * endpoint and reads the response from the interrupt endpoint, then
 * exposes the battery level via the power_supply subsystem.
 *
 * Supported devices:
 *   - Keychron M5 (wired mode): 3434:d048
 *   - Keychron M5 (wireless via Ultra-Link 8K receiver): 3434:d028
 */

#include <linux/module.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/delay.h>

#define USB_VENDOR_ID_KEYCHRON		0x3434
#define USB_DEVICE_ID_KEYCHRON_M5	0xd048
#define USB_DEVICE_ID_KEYCHRON_RECV	0xd028

#define KEYCHRON_REPORT_ID_CMD		0xB3
#define KEYCHRON_REPORT_ID_RESP		0xB4
#define KEYCHRON_CMD_STATUS		0x06
#define KEYCHRON_BATTERY_OFFSET		20
#define KEYCHRON_VENDOR_INTERFACE	4

#define KEYCHRON_POLL_INTERVAL_MS	300000	/* 5 minutes */
#define KEYCHRON_USB_TIMEOUT_MS		1000
#define KEYCHRON_RESPONSE_TIMEOUT_MS	500
#define KEYCHRON_REPORT_SIZE		64
#define KEYCHRON_QUERY_RETRIES		3
#define KEYCHRON_RETRY_DELAY_MS		100

/*
 * Global state for ensuring only one battery instance exists.
 * Multiple HID interfaces probe for the same physical device.
 */
static struct keychron_device *keychron_battery_owner;
static DEFINE_MUTEX(keychron_battery_mutex);

struct keychron_device {
	struct hid_device *hdev;
	struct usb_device *udev;
	struct usb_interface *intf;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	struct delayed_work battery_work;
	struct urb *intr_urb;
	struct completion response_received;
	u8 *intr_buf;
	int intr_ep;
	int intr_interval;
	int battery_capacity;
	int pending_battery;
	bool owns_battery;
	atomic_t waiting_response;
};

static enum power_supply_property keychron_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int keychron_battery_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct keychron_device *kdev = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = kdev->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (kdev->battery_capacity >= 80)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		else if (kdev->battery_capacity >= 40)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		else if (kdev->battery_capacity >= 10)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Keychron M5";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Keychron";
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void keychron_urb_complete(struct urb *urb)
{
	struct keychron_device *kdev = urb->context;
	u8 *data = kdev->intr_buf;

	if (urb->status)
		return;

	if (!atomic_read(&kdev->waiting_response))
		return;

	/* Validate response: report ID 0xB4, command echo 0x06, valid length */
	if (urb->actual_length >= KEYCHRON_BATTERY_OFFSET + 1 &&
	    data[0] == KEYCHRON_REPORT_ID_RESP &&
	    data[1] == KEYCHRON_CMD_STATUS &&
	    data[KEYCHRON_BATTERY_OFFSET] <= 100) {
		kdev->pending_battery = data[KEYCHRON_BATTERY_OFFSET];
		complete(&kdev->response_received);
	}
}

static int keychron_query_battery_once(struct keychron_device *kdev, u8 *buf)
{
	int ret;
	int intf_num;
	unsigned long timeout;

	/* Prepare for interrupt response */
	reinit_completion(&kdev->response_received);
	kdev->pending_battery = -1;
	atomic_set(&kdev->waiting_response, 1);

	/* Submit URB to receive interrupt response */
	usb_fill_int_urb(kdev->intr_urb, kdev->udev,
			 usb_rcvintpipe(kdev->udev, kdev->intr_ep),
			 kdev->intr_buf, KEYCHRON_REPORT_SIZE,
			 keychron_urb_complete, kdev,
			 kdev->intr_interval);

	ret = usb_submit_urb(kdev->intr_urb, GFP_KERNEL);
	if (ret < 0)
		goto out;

	/* Send status request via control endpoint (SET_REPORT feature) */
	memset(buf, 0, KEYCHRON_REPORT_SIZE);
	buf[0] = KEYCHRON_REPORT_ID_CMD;
	buf[1] = KEYCHRON_CMD_STATUS;

	intf_num = kdev->intf->cur_altsetting->desc.bInterfaceNumber;

	ret = usb_control_msg(kdev->udev,
			      usb_sndctrlpipe(kdev->udev, 0),
			      HID_REQ_SET_REPORT,
			      USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      (HID_FEATURE_REPORT << 8) | KEYCHRON_REPORT_ID_CMD,
			      intf_num,
			      buf, KEYCHRON_REPORT_SIZE,
			      KEYCHRON_USB_TIMEOUT_MS);
	if (ret < 0) {
		usb_kill_urb(kdev->intr_urb);
		goto out;
	}

	/* Wait for interrupt response */
	timeout = wait_for_completion_timeout(&kdev->response_received,
			msecs_to_jiffies(KEYCHRON_RESPONSE_TIMEOUT_MS));

	usb_kill_urb(kdev->intr_urb);

	if (timeout && kdev->pending_battery >= 0)
		return kdev->pending_battery;

	return -ETIMEDOUT;

out:
	atomic_set(&kdev->waiting_response, 0);
	return ret;
}

static int keychron_query_battery(struct keychron_device *kdev)
{
	u8 *buf;
	int ret;
	int attempt;

	if (!kdev->udev || !kdev->intr_urb)
		return -ENODEV;

	buf = kmalloc(KEYCHRON_REPORT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (attempt = 0; attempt < KEYCHRON_QUERY_RETRIES; attempt++) {
		if (attempt > 0)
			msleep(KEYCHRON_RETRY_DELAY_MS);

		ret = keychron_query_battery_once(kdev, buf);
		atomic_set(&kdev->waiting_response, 0);

		if (ret >= 0)
			break;
	}

	if (ret < 0 && attempt == KEYCHRON_QUERY_RETRIES)
		hid_dbg(kdev->hdev, "battery query failed after %d attempts\n",
			KEYCHRON_QUERY_RETRIES);

	kfree(buf);
	return ret;
}

static void keychron_battery_work(struct work_struct *work)
{
	struct keychron_device *kdev = container_of(work, struct keychron_device,
						    battery_work.work);
	int battery;

	battery = keychron_query_battery(kdev);
	if (battery >= 0 && battery != kdev->battery_capacity) {
		kdev->battery_capacity = battery;
		power_supply_changed(kdev->battery);
		hid_dbg(kdev->hdev, "battery: %d%%\n", battery);
	}

	schedule_delayed_work(&kdev->battery_work,
			      msecs_to_jiffies(KEYCHRON_POLL_INTERVAL_MS));
}

static bool keychron_is_vendor_interface(struct hid_device *hdev)
{
	struct usb_interface *intf;

	if (!hid_is_usb(hdev))
		return false;

	intf = to_usb_interface(hdev->dev.parent);
	return intf->cur_altsetting->desc.bInterfaceNumber ==
	       KEYCHRON_VENDOR_INTERFACE;
}

static int keychron_find_intr_endpoint(struct usb_interface *intf,
				       int *ep_addr, int *interval)
{
	struct usb_endpoint_descriptor *ep;
	int i;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep = &intf->cur_altsetting->endpoint[i].desc;
		if (usb_endpoint_is_int_in(ep)) {
			*ep_addr = ep->bEndpointAddress;
			*interval = ep->bInterval;
			return 0;
		}
	}
	return -ENOENT;
}

static void keychron_cleanup_battery(struct keychron_device *kdev)
{
	kfree(kdev->intr_buf);
	kdev->intr_buf = NULL;
	usb_free_urb(kdev->intr_urb);
	kdev->intr_urb = NULL;
	usb_put_dev(kdev->udev);
	kdev->udev = NULL;

	mutex_lock(&keychron_battery_mutex);
	keychron_battery_owner = NULL;
	kdev->owns_battery = false;
	mutex_unlock(&keychron_battery_mutex);
}

static int keychron_probe(struct hid_device *hdev,
			  const struct hid_device_id *id)
{
	struct keychron_device *kdev;
	struct usb_interface *intf;
	struct power_supply_config psy_cfg = {};
	int ret;
	int battery;

	kdev = devm_kzalloc(&hdev->dev, sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return -ENOMEM;

	kdev->hdev = hdev;
	kdev->battery_capacity = 0;
	kdev->owns_battery = false;
	init_completion(&kdev->response_received);
	atomic_set(&kdev->waiting_response, 0);
	hid_set_drvdata(hdev, kdev);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	/* Only handle battery on the vendor interface */
	if (!keychron_is_vendor_interface(hdev))
		return 0;

	mutex_lock(&keychron_battery_mutex);
	if (keychron_battery_owner) {
		mutex_unlock(&keychron_battery_mutex);
		return 0;
	}
	keychron_battery_owner = kdev;
	kdev->owns_battery = true;
	mutex_unlock(&keychron_battery_mutex);

	/* Get USB device and interface */
	intf = to_usb_interface(hdev->dev.parent);
	kdev->intf = intf;
	kdev->udev = usb_get_dev(interface_to_usbdev(intf));

	/* Find interrupt endpoint */
	ret = keychron_find_intr_endpoint(intf, &kdev->intr_ep,
					  &kdev->intr_interval);
	if (ret) {
		hid_err(hdev, "no interrupt endpoint found\n");
		goto err_cleanup;
	}

	/* Allocate URB and buffer */
	kdev->intr_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!kdev->intr_urb) {
		ret = -ENOMEM;
		goto err_cleanup;
	}

	kdev->intr_buf = kmalloc(KEYCHRON_REPORT_SIZE, GFP_KERNEL);
	if (!kdev->intr_buf) {
		ret = -ENOMEM;
		goto err_cleanup;
	}

	/* Test battery query */
	battery = keychron_query_battery(kdev);
	if (battery < 0) {
		hid_info(hdev, "battery query failed (%d), device may not support battery reporting\n",
			 battery);
		ret = 0; /* Don't fail probe, just skip battery */
		goto err_cleanup;
	}

	kdev->battery_capacity = battery;

	/* Register power supply */
	kdev->battery_desc.name = "keychron_mouse";
	kdev->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	kdev->battery_desc.properties = keychron_battery_props;
	kdev->battery_desc.num_properties = ARRAY_SIZE(keychron_battery_props);
	kdev->battery_desc.get_property = keychron_battery_get_property;

	psy_cfg.drv_data = kdev;

	kdev->battery = power_supply_register(&hdev->dev, &kdev->battery_desc,
					      &psy_cfg);
	if (IS_ERR(kdev->battery)) {
		ret = PTR_ERR(kdev->battery);
		hid_err(hdev, "failed to register power supply: %d\n", ret);
		goto err_cleanup;
	}

	INIT_DELAYED_WORK(&kdev->battery_work, keychron_battery_work);
	schedule_delayed_work(&kdev->battery_work,
			      msecs_to_jiffies(KEYCHRON_POLL_INTERVAL_MS));

	hid_info(hdev, "Keychron mouse battery: %d%%\n", battery);
	return 0;

err_cleanup:
	keychron_cleanup_battery(kdev);
	return ret;
}

static void keychron_remove(struct hid_device *hdev)
{
	struct keychron_device *kdev = hid_get_drvdata(hdev);

	if (kdev && kdev->owns_battery) {
		cancel_delayed_work_sync(&kdev->battery_work);
		usb_kill_urb(kdev->intr_urb);
		power_supply_unregister(kdev->battery);
		keychron_cleanup_battery(kdev);
	}

	hid_hw_stop(hdev);
}

static const struct hid_device_id keychron_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_M5) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_RECV) },
	{ }
};
MODULE_DEVICE_TABLE(hid, keychron_devices);

static struct hid_driver keychron_driver = {
	.name = "keychron",
	.id_table = keychron_devices,
	.probe = keychron_probe,
	.remove = keychron_remove,
};
module_hid_driver(keychron_driver);

MODULE_AUTHOR("Chris Sutcliff <chris@sutcliff.me>");
MODULE_DESCRIPTION("HID driver for Keychron mouse battery reporting");
MODULE_LICENSE("GPL");
