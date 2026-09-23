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
 *   - Keychron M6 (wired mode): 3434:d049
 *   - Keychron M6 (wireless via Ultra-Link 8K receiver): 3434:d028
 *
 * The receiver has the same USB ID for every mouse, so when connected
 * through it the model is found by asking the receiver for the VID/PID of
 * the paired mouse. This is what Keychron's web launcher does too.
 */

#include <linux/module.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/unaligned.h>

#define USB_VENDOR_ID_KEYCHRON		0x3434
#define USB_DEVICE_ID_KEYCHRON_M5	0xd048
#define USB_DEVICE_ID_KEYCHRON_M6	0xd049
#define USB_DEVICE_ID_KEYCHRON_RECV	0xd028

#define KEYCHRON_REPORT_ID_CMD		0xB3
#define KEYCHRON_REPORT_ID_RESP		0xB4
#define KEYCHRON_CMD_STATUS		0x06
#define KEYCHRON_BATTERY_OFFSET		20
#define KEYCHRON_BATTERY_CHARGING	BIT(7)
#define KEYCHRON_BATTERY_LEVEL		GENMASK(6, 0)
#define KEYCHRON_VENDOR_INTERFACE	4

#define KEYCHRON_REPORT_ID_INFO		0xB5
#define KEYCHRON_CMD_RECV_STATE		0x03
#define KEYCHRON_INFO_REPORT_SIZE	21
#define KEYCHRON_RECV_SLOT_OFFSET	3
#define KEYCHRON_RECV_SLOT_SIZE		5
#define KEYCHRON_RECV_SLOTS		3

#define KEYCHRON_POLL_INTERVAL_MS	300000	/* 5 minutes */
#define KEYCHRON_RETRY_POLL_MS		60000	/* 1 minute, while mouse not yet responding */
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
	struct delayed_work work;
	struct urb *intr_urb;
	struct completion response_received;
	u8 *intr_buf;
	int intr_ep;
	int intr_interval;
	int battery_capacity;
	bool battery_charging;
	u8 expected_cmd;
	u8 expected_resp_id;
	u8 resp[KEYCHRON_REPORT_SIZE];
	int resp_len;
	const char *model_name;
	bool owns_battery;
	atomic_t waiting_response;
};

/* driver_data holds the model name, NULL where it must be queried */
static const struct hid_device_id keychron_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_M5),
	  .driver_data = (kernel_ulong_t)"Keychron M5" },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_M6),
	  .driver_data = (kernel_ulong_t)"Keychron M6 8K" },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_RECV) },
	{ }
};
MODULE_DEVICE_TABLE(hid, keychron_devices);

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
		if (!kdev->battery_charging)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (kdev->battery_capacity == 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
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
		val->strval = kdev->model_name;
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
	int ret;

	if (urb->status)
		return;

	if (!atomic_read(&kdev->waiting_response))
		return;

	/* Validate response: command echo and, if known, the report ID */
	if (urb->actual_length < 2 || data[1] != kdev->expected_cmd ||
	    (kdev->expected_resp_id && data[0] != kdev->expected_resp_id)) {
		hid_dbg(kdev->hdev,
			"skipping report (expected: cmd=0x%02x, resp_id=0x%02x): %*ph\n",
			kdev->expected_cmd, kdev->expected_resp_id,
			urb->actual_length, data);

		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret)
			hid_dbg(kdev->hdev, "failed to re-submit URB: %d\n",
				ret);

		return;
	}

	kdev->resp_len = urb->actual_length;
	memcpy(kdev->resp, data, urb->actual_length);
	complete(&kdev->response_received);
}

static int keychron_query_once(struct keychron_device *kdev, u8 *buf,
			       u8 report_id, u8 cmd, u8 resp_id, size_t len)
{
	int ret;
	unsigned long timeout;

	/* Prepare command buffer */
	memset(buf, 0, len);
	buf[0] = report_id;
	buf[1] = cmd;

	/* Prepare for interrupt response */
	reinit_completion(&kdev->response_received);
	kdev->expected_cmd = cmd;
	kdev->expected_resp_id = resp_id;
	kdev->resp_len = 0;
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

	ret = hid_hw_raw_request(kdev->hdev, report_id, buf, len,
				 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		usb_kill_urb(kdev->intr_urb);
		goto out;
	}

	/* Wait for interrupt response */
	timeout = wait_for_completion_timeout(&kdev->response_received,
			msecs_to_jiffies(KEYCHRON_RESPONSE_TIMEOUT_MS));

	usb_kill_urb(kdev->intr_urb);

	if (timeout && kdev->resp_len)
		return kdev->resp_len;

	return -ETIMEDOUT;

out:
	atomic_set(&kdev->waiting_response, 0);
	return ret;
}

static int keychron_query(struct keychron_device *kdev, u8 report_id, u8 cmd,
			  u8 resp_id, size_t len)
{
	u8 *buf;
	int ret;
	int attempt;

	if (!kdev->udev || !kdev->intr_urb)
		return -ENODEV;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (attempt = 0; attempt < KEYCHRON_QUERY_RETRIES; attempt++) {
		if (attempt > 0)
			msleep(KEYCHRON_RETRY_DELAY_MS);

		ret = keychron_query_once(kdev, buf, report_id, cmd, resp_id,
					  len);
		atomic_set(&kdev->waiting_response, 0);

		if (ret >= 0)
			break;
	}

	if (ret < 0 && attempt == KEYCHRON_QUERY_RETRIES)
		hid_dbg(kdev->hdev, "query 0x%02x/0x%02x failed after %d attempts\n",
			report_id, cmd, KEYCHRON_QUERY_RETRIES);

	kfree(buf);
	return ret;
}

static int keychron_query_battery_status(struct keychron_device *kdev)
{
	int ret;

	ret = keychron_query(kdev, KEYCHRON_REPORT_ID_CMD, KEYCHRON_CMD_STATUS,
			     KEYCHRON_REPORT_ID_RESP, KEYCHRON_REPORT_SIZE);
	if (ret < 0)
		return ret;

	if (ret < KEYCHRON_BATTERY_OFFSET + 1 ||
	    (kdev->resp[KEYCHRON_BATTERY_OFFSET] & KEYCHRON_BATTERY_LEVEL) > 100)
		return -EPROTO;

	return kdev->resp[KEYCHRON_BATTERY_OFFSET];
}

static bool keychron_update_battery(struct keychron_device *kdev, int battery)
{
	int capacity = battery & KEYCHRON_BATTERY_LEVEL;
	bool charging = !!(battery & KEYCHRON_BATTERY_CHARGING);
	bool changed = capacity != kdev->battery_capacity ||
		       charging != kdev->battery_charging;

	kdev->battery_capacity = capacity;
	kdev->battery_charging = charging;

	return changed;
}

static int keychron_query_paired_product(struct keychron_device *kdev,
					 struct hid_device_id *paired_device_id)
{
	const u8 *slot;
	bool connected;
	u8 i, off;
	int ret;

	ret = keychron_query(kdev, KEYCHRON_REPORT_ID_INFO,
			     KEYCHRON_CMD_RECV_STATE, 0,
			     KEYCHRON_INFO_REPORT_SIZE);
	if (ret < 0)
		return ret;

	for (i = 0; i < KEYCHRON_RECV_SLOTS; i++) {
		off = KEYCHRON_RECV_SLOT_OFFSET + (i * KEYCHRON_RECV_SLOT_SIZE);
		if (off + KEYCHRON_RECV_SLOT_SIZE > ret)
			break;

		slot = &kdev->resp[off];

		connected = slot[4] == 1;
		if (!connected)
			continue;

		paired_device_id->vendor = get_unaligned_le16(&slot[0]);
		paired_device_id->product = get_unaligned_le16(&slot[2]);

		return 0;
	}

	return -ENODEV;
}

static int keychron_detect_model(struct keychron_device *kdev)
{
	struct hid_device_id paired_device_id = { HID_USB_DEVICE(0, 0) };
	const struct hid_device_id *id;
	int ret;

	if (kdev->model_name)
		return 0;

	if (kdev->hdev->product == USB_DEVICE_ID_KEYCHRON_RECV) {
		ret = keychron_query_paired_product(kdev, &paired_device_id);
		if (ret) {
			hid_dbg(kdev->hdev,
				"failed to query paired mouse: %d\n",
				ret);
			return ret;
		}

		/* fallback, in case we don't find a match */
		kdev->model_name = "Keychron Ultra-Link 8K";

		for (id = keychron_devices; id->bus; id++) {
			if (id->bus == paired_device_id.bus &&
			    id->vendor == paired_device_id.vendor &&
			    id->product == paired_device_id.product &&
			    id->driver_data) {
				kdev->model_name = (const char *)id->driver_data;
				break;
			}
		}

		return 0;
	}

	id = hid_match_id(kdev->hdev, keychron_devices);
	if (id)
		kdev->model_name = (const char *)id->driver_data;
	else
		kdev->model_name = "Keychron (Unknown)";

	return 0;
}

static int keychron_register_battery(struct keychron_device *kdev)
{
	struct power_supply_config psy_cfg = {};

	kdev->battery_desc.name = "keychron_mouse";
	kdev->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	kdev->battery_desc.properties = keychron_battery_props;
	kdev->battery_desc.num_properties = ARRAY_SIZE(keychron_battery_props);
	kdev->battery_desc.get_property = keychron_battery_get_property;

	psy_cfg.drv_data = kdev;

	kdev->battery = power_supply_register(&kdev->hdev->dev,
					      &kdev->battery_desc, &psy_cfg);
	if (IS_ERR(kdev->battery)) {
		int ret = PTR_ERR(kdev->battery);

		hid_err(kdev->hdev, "failed to register power supply: %d\n", ret);
		kdev->battery = NULL;
		return ret;
	}

	return 0;
}

static void keychron_work(struct work_struct *work)
{
	struct keychron_device *kdev = container_of(work, struct keychron_device,
						    work.work);
	int ret, battery_status;

	ret = keychron_detect_model(kdev);
	if (ret)
		battery_status = ret;
	else
		battery_status = keychron_query_battery_status(kdev);

	/*
	 * The mouse is wireless and frequently asleep (notably at boot), so a
	 * failed query is expected and transient. Keep polling on a shorter
	 * interval until it responds rather than giving up — otherwise a single
	 * miss leaves the battery unreported for the whole session.
	 */
	if (battery_status < 0) {
		schedule_delayed_work(&kdev->work,
				      msecs_to_jiffies(KEYCHRON_RETRY_POLL_MS));
		return;
	}

	/* First successful reading: register the power supply now. */
	if (!kdev->battery) {
		keychron_update_battery(kdev, battery_status);
		if (keychron_register_battery(kdev)) {
			schedule_delayed_work(&kdev->work,
					      msecs_to_jiffies(KEYCHRON_RETRY_POLL_MS));
			return;
		}
		hid_info(kdev->hdev, "%s battery: %d%%%s\n", kdev->model_name,
			 kdev->battery_capacity,
			 kdev->battery_charging ? " (charging)" : "");
	} else if (keychron_update_battery(kdev, battery_status)) {
		power_supply_changed(kdev->battery);
		hid_dbg(kdev->hdev, "battery: %d%%%s\n", kdev->battery_capacity,
			kdev->battery_charging ? " (charging)" : "");
	}

	schedule_delayed_work(&kdev->work,
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
	int battery, ret;

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

	/*
	 * Start standard interfaces with full connectivity (keyboard, mouse
	 * input devices etc). The vendor interface only needs hidraw — creating
	 * input devices from its HID descriptor exposes keyboard capabilities
	 * that cause UPower to misclassify the battery as a keyboard.
	 */
	if (keychron_is_vendor_interface(hdev))
		ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	else
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

	INIT_DELAYED_WORK(&kdev->work, keychron_work);

	/*
	 * Try an initial query so the battery shows up immediately when the
	 * mouse is awake. A failure here is expected for a sleeping wireless
	 * mouse and must NOT abort: the poll worker keeps retrying and
	 * registers the power supply on the first successful reading.
	 */
	ret = keychron_detect_model(kdev);
	if (ret)
		battery = ret;
	else
		battery = keychron_query_battery_status(kdev);

	if (battery >= 0) {
		keychron_update_battery(kdev, battery);
		ret = keychron_register_battery(kdev);
		if (ret)
			goto err_cleanup;
		hid_info(hdev, "%s battery: %d%%%s\n", kdev->model_name,
			 kdev->battery_capacity,
			 kdev->battery_charging ? " (charging)" : "");
		schedule_delayed_work(&kdev->work,
				      msecs_to_jiffies(KEYCHRON_POLL_INTERVAL_MS));
	} else {
		hid_info(hdev, "initial query failed (%d), mouse likely asleep; will keep polling\n",
			 battery);
		schedule_delayed_work(&kdev->work,
				      msecs_to_jiffies(KEYCHRON_RETRY_POLL_MS));
	}

	return 0;

err_cleanup:
	keychron_cleanup_battery(kdev);
	return ret;
}

static void keychron_remove(struct hid_device *hdev)
{
	struct keychron_device *kdev = hid_get_drvdata(hdev);

	if (kdev && kdev->owns_battery) {
		cancel_delayed_work_sync(&kdev->work);
		usb_kill_urb(kdev->intr_urb);
		if (kdev->battery)
			power_supply_unregister(kdev->battery);
		keychron_cleanup_battery(kdev);
	}

	hid_hw_stop(hdev);
}

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
