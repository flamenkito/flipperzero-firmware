#include <furi_hal_rtc.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_usb_spoof.h>

#include "furi_hal_usb_i.h"
#include "usb_hid.h"

#define HID_PAGE_VENDOR   0xFF00
#define HID_VENDOR_USAGE  0x01
#define HID_VENDOR_INPUT  0x20
#define HID_VENDOR_OUTPUT 0x21

static const uint8_t hid_vendor_report_desc[] = {
    HID_RI_USAGE_PAGE(16, HID_PAGE_VENDOR),
    HID_USAGE(HID_VENDOR_USAGE),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_USAGE(HID_VENDOR_INPUT),
    HID_LOGICAL_MINIMUM(0x00),
    HID_RI_LOGICAL_MAXIMUM(16, 0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(HID_VENDOR_PACKET_LEN),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_USAGE(HID_VENDOR_OUTPUT),
    HID_LOGICAL_MINIMUM(0x00),
    HID_RI_LOGICAL_MAXIMUM(16, 0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(HID_VENDOR_PACKET_LEN),
    HID_OUTPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
};

static const uint8_t hid_keyboard_report_desc[] = {
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_KEYBOARD),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_USAGE_PAGE(HID_DESKTOP_KEYPAD),
    HID_USAGE_MINIMUM(HID_KEYBOARD_L_CTRL),
    HID_USAGE_MAXIMUM(HID_KEYBOARD_R_GUI),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(1),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(8),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(1),
    HID_REPORT_SIZE(8),
    HID_INPUT(HID_IOF_CONSTANT | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(5),
    HID_REPORT_SIZE(1),
    HID_USAGE_PAGE(HID_PAGE_LED),
    HID_USAGE_MINIMUM(1),
    HID_USAGE_MAXIMUM(5),
    HID_OUTPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(1),
    HID_REPORT_SIZE(3),
    HID_OUTPUT(HID_IOF_CONSTANT | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(HID_KB_MAX_KEYS),
    HID_REPORT_SIZE(8),
    HID_LOGICAL_MINIMUM(0),
    HID_RI_LOGICAL_MAXIMUM(16, 0xFF),
    HID_USAGE_PAGE(HID_DESKTOP_KEYPAD),
    HID_USAGE_MINIMUM(0),
    HID_RI_USAGE_MAXIMUM(16, 0xFF),
    HID_INPUT(HID_IOF_DATA | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
};

_Static_assert(
    sizeof(hid_vendor_report_desc) == FURI_HAL_USB_SPOOF_VENDOR_REPORT_DESC_LEN,
    "vendor report descriptor length mismatch");
_Static_assert(
    sizeof(hid_keyboard_report_desc) == FURI_HAL_USB_SPOOF_KEYBOARD_REPORT_DESC_LEN,
    "keyboard report descriptor length mismatch");

static const struct usb_string_descriptor logitech_manuf_desc = USB_STRING_DESC("Logitech");
static const struct usb_string_descriptor logitech_prod_desc = USB_STRING_DESC("USB Keyboard");
static const struct usb_string_descriptor dell_manuf_desc = USB_STRING_DESC("Dell");
static const struct usb_string_descriptor dell_prod_desc = USB_STRING_DESC("KB216 Keyboard");

#define USB_SPOOF_DEVICE_DESCRIPTOR(vid, pid)                           \
    {                                                                  \
        .bLength = sizeof(struct usb_device_descriptor),                \
        .bDescriptorType = USB_DTYPE_DEVICE,                            \
        .bcdUSB = VERSION_BCD(2, 0, 0),                                 \
        .bDeviceClass = USB_CLASS_IAD,                                  \
        .bDeviceSubClass = USB_SUBCLASS_IAD,                            \
        .bDeviceProtocol = USB_PROTO_IAD,                               \
        .bMaxPacketSize0 = USB_EP0_SIZE,                                \
        .idVendor = vid,                                                \
        .idProduct = pid,                                               \
        .bcdDevice = VERSION_BCD(1, 0, 0),                              \
        .iManufacturer = UsbDevManuf,                                   \
        .iProduct = UsbDevProduct,                                      \
        .iSerialNumber = 0,                                             \
        .bNumConfigurations = 1,                                        \
    }

static const struct usb_device_descriptor logitech_device_desc =
    USB_SPOOF_DEVICE_DESCRIPTOR(0x046D, 0xC31C);
static const struct usb_device_descriptor dell_device_desc =
    USB_SPOOF_DEVICE_DESCRIPTOR(0x413C, 0x2113);

static const HidCompositeConfigDescriptor hid_composite_cfg_desc = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(HidCompositeConfigDescriptor),
            .bNumInterfaces = 2,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED | USB_CFG_ATTR_SELFPOWERED,
            .bMaxPower = USB_CFG_POWER_MA(500),
        },
    .hid_iad =
        {
            .bLength = sizeof(struct usb_iad_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFASEASSOC,
            .bFirstInterface = 0,
            .bInterfaceCount = 2,
            .bFunctionClass = USB_CLASS_HID,
            .bFunctionSubClass = USB_HID_SUBCLASS_BOOT,
            .bFunctionProtocol = USB_HID_PROTO_KEYBOARD,
            .iFunction = NO_DESCRIPTOR,
        },
    .keyboard =
        {
            .hid =
                {
                    .bLength = sizeof(struct usb_interface_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFACE,
                    .bInterfaceNumber = 0,
                    .bAlternateSetting = 0,
                    .bNumEndpoints = 1,
                    .bInterfaceClass = USB_CLASS_HID,
                    .bInterfaceSubClass = USB_HID_SUBCLASS_BOOT,
                    .bInterfaceProtocol = USB_HID_PROTO_KEYBOARD,
                    .iInterface = NO_DESCRIPTOR,
                },
            .hid_desc =
                {
                    .bLength = sizeof(struct usb_hid_descriptor),
                    .bDescriptorType = USB_DTYPE_HID,
                    .bcdHID = VERSION_BCD(1, 0, 0),
                    .bCountryCode = USB_HID_COUNTRY_NONE,
                    .bNumDescriptors = 1,
                    .bDescriptorType0 = USB_DTYPE_HID_REPORT,
                    .wDescriptorLength0 = sizeof(hid_keyboard_report_desc),
                },
            .hid_ep_in =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = HID_KBD_EP_IN,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = HID_KBD_PACKET_LEN,
                    .bInterval = HID_INTERVAL,
                },
        },
    .vendor =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = 1,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_HID,
            .bInterfaceSubClass = USB_HID_SUBCLASS_NONBOOT,
            .bInterfaceProtocol = USB_HID_PROTO_NONBOOT,
            .iInterface = NO_DESCRIPTOR,
        },
    .vendor_hid_desc =
        {
            .bLength = sizeof(struct usb_hid_descriptor),
            .bDescriptorType = USB_DTYPE_HID,
            .bcdHID = VERSION_BCD(1, 0, 0),
            .bCountryCode = USB_HID_COUNTRY_NONE,
            .bNumDescriptors = 1,
            .bDescriptorType0 = USB_DTYPE_HID_REPORT,
            .wDescriptorLength0 = sizeof(hid_vendor_report_desc),
        },
    .vendor_ep_in =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = HID_VENDOR_EP_IN,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = HID_VENDOR_PACKET_LEN,
            .bInterval = HID_INTERVAL,
        },
    .vendor_ep_out =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = HID_VENDOR_EP_OUT,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = HID_VENDOR_PACKET_LEN,
            .bInterval = HID_INTERVAL,
        },
};

_Static_assert(sizeof(HidCompositeConfigDescriptor) == 74, "composite descriptor size mismatch");

static void usb_spoof_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx);
static void usb_spoof_deinit(usbd_device* dev);
static void usb_spoof_wakeup(usbd_device* dev);
static void usb_spoof_suspend(usbd_device* dev);
static usbd_respond usb_spoof_ep_config(usbd_device* dev, uint8_t cfg);
static usbd_respond
    usb_spoof_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback);

static FuriHalUsbInterface usb_spoof_logitech = {
    .init = usb_spoof_init,
    .deinit = usb_spoof_deinit,
    .wakeup = usb_spoof_wakeup,
    .suspend = usb_spoof_suspend,
    .dev_descr = (struct usb_device_descriptor*)&logitech_device_desc,
    .str_manuf_descr = (void*)&logitech_manuf_desc,
    .str_prod_descr = (void*)&logitech_prod_desc,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)&hid_composite_cfg_desc,
};

static FuriHalUsbInterface usb_spoof_dell = {
    .init = usb_spoof_init,
    .deinit = usb_spoof_deinit,
    .wakeup = usb_spoof_wakeup,
    .suspend = usb_spoof_suspend,
    .dev_descr = (struct usb_device_descriptor*)&dell_device_desc,
    .str_manuf_descr = (void*)&dell_manuf_desc,
    .str_prod_descr = (void*)&dell_prod_desc,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)&hid_composite_cfg_desc,
};

static const FuriHalUsbSpoofIdentity usb_spoof_identities[] = {
    [FuriHalUsbSpoofProfileLogitech] =
        {
            .device_desc = &logitech_device_desc,
            .manuf_desc = &logitech_manuf_desc,
            .prod_desc = &logitech_prod_desc,
            .composite_config_desc = &hid_composite_cfg_desc,
            .keyboard_hid_desc = &hid_composite_cfg_desc.keyboard.hid_desc,
            .keyboard_hid_desc_len = sizeof(hid_composite_cfg_desc.keyboard.hid_desc),
            .vendor_hid_desc = &hid_composite_cfg_desc.vendor_hid_desc,
            .vendor_hid_desc_len = sizeof(hid_composite_cfg_desc.vendor_hid_desc),
        },
    [FuriHalUsbSpoofProfileDell] =
        {
            .device_desc = &dell_device_desc,
            .manuf_desc = &dell_manuf_desc,
            .prod_desc = &dell_prod_desc,
            .composite_config_desc = &hid_composite_cfg_desc,
            .keyboard_hid_desc = &hid_composite_cfg_desc.keyboard.hid_desc,
            .keyboard_hid_desc_len = sizeof(hid_composite_cfg_desc.keyboard.hid_desc),
            .vendor_hid_desc = &hid_composite_cfg_desc.vendor_hid_desc,
            .vendor_hid_desc_len = sizeof(hid_composite_cfg_desc.vendor_hid_desc),
        },
};

static FuriHalUsbSpoofProfile active_profile = FuriHalUsbSpoofProfileLogitech;
static bool active_latched;

static FuriHalUsbSpoofProfile usb_spoof_normalize_profile(FuriHalUsbSpoofProfile profile) {
    return (profile == FuriHalUsbSpoofProfileDell) ? FuriHalUsbSpoofProfileDell :
                                                    FuriHalUsbSpoofProfileLogitech;
}

FuriHalUsbInterface* furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfile profile) {
    profile = usb_spoof_normalize_profile(profile);
    return (profile == FuriHalUsbSpoofProfileDell) ? &usb_spoof_dell : &usb_spoof_logitech;
}

const FuriHalUsbSpoofIdentity*
    furi_hal_usb_spoof_get_identity(FuriHalUsbSpoofProfile profile) {
    return &usb_spoof_identities[usb_spoof_normalize_profile(profile)];
}

const uint8_t* furi_hal_usb_spoof_vendor_report_desc(uint16_t* len) {
    if(len != NULL) {
        *len = sizeof(hid_vendor_report_desc);
    }
    return hid_vendor_report_desc;
}

const uint8_t* furi_hal_usb_spoof_keyboard_report_desc(uint16_t* len) {
    if(len != NULL) {
        *len = sizeof(hid_keyboard_report_desc);
    }
    return hid_keyboard_report_desc;
}

void furi_hal_usb_spoof_latch_active(FuriHalUsbSpoofProfile profile) {
    active_profile = usb_spoof_normalize_profile(profile);
    active_latched = true;
}

FuriHalUsbInterface* furi_hal_usb_spoof_get_active_interface(void) {
    if(active_latched) {
        return furi_hal_usb_spoof_get_interface(active_profile);
    }

    /* Before desktop latches the boot choice, RTC is the only available source.
     * Settings cannot change it before desktop starts, so this fallback still
     * resolves to the boot identity and never causes a live profile switch. */
    return furi_hal_usb_spoof_get_interface(
        (FuriHalUsbSpoofProfile)furi_hal_rtc_get_usb_identity());
}

static void usb_spoof_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(intf);
    UNUSED(ctx);
    usbd_reg_config(dev, usb_spoof_ep_config);
    usbd_reg_control(dev, usb_spoof_control);
    usbd_connect(dev, true);
}

static void usb_spoof_deinit(usbd_device* dev) {
    usb_spoof_ep_config(dev, 0);
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
}

static void usb_spoof_wakeup(usbd_device* dev) {
    UNUSED(dev);
}

static void usb_spoof_suspend(usbd_device* dev) {
    UNUSED(dev);
}

static usbd_respond usb_spoof_ep_config(usbd_device* dev, uint8_t cfg) {
    switch(cfg) {
    case 0:
        usbd_ep_deconfig(dev, HID_VENDOR_EP_OUT);
        usbd_ep_deconfig(dev, HID_VENDOR_EP_IN);
        usbd_ep_deconfig(dev, HID_KBD_EP_IN);
        usbd_reg_endpoint(dev, HID_VENDOR_EP_OUT, NULL);
        usbd_reg_endpoint(dev, HID_VENDOR_EP_IN, NULL);
        usbd_reg_endpoint(dev, HID_KBD_EP_IN, NULL);
        return usbd_ack;
    case 1:
        usbd_ep_config(dev, HID_KBD_EP_IN, USB_EPTYPE_INTERRUPT, HID_KBD_PACKET_LEN);
        usbd_ep_config(dev, HID_VENDOR_EP_IN, USB_EPTYPE_INTERRUPT, HID_VENDOR_PACKET_LEN);
        usbd_ep_config(dev, HID_VENDOR_EP_OUT, USB_EPTYPE_INTERRUPT, HID_VENDOR_PACKET_LEN);
        usbd_reg_endpoint(dev, HID_KBD_EP_IN, NULL);
        usbd_reg_endpoint(dev, HID_VENDOR_EP_IN, NULL);
        usbd_reg_endpoint(dev, HID_VENDOR_EP_OUT, NULL);
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

static usbd_respond
    usb_spoof_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    static const uint8_t keyboard_report[HID_KBD_PACKET_LEN] = {0};

    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_CLASS) &&
       (req->wIndex <= 1)) {
        switch(req->bRequest) {
        case USB_HID_SETIDLE:
        case USB_HID_SETPROTOCOL:
            return usbd_ack;
        case USB_HID_GETREPORT:
            if(req->wIndex == 0) {
                dev->status.data_ptr = (uint8_t*)keyboard_report;
                dev->status.data_count = sizeof(keyboard_report);
                return usbd_ack;
            }
            return usbd_fail;
        default:
            return usbd_fail;
        }
    }

    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_STANDARD) &&
       req->bRequest == USB_STD_GET_DESCRIPTOR) {
        switch(req->wValue >> 8) {
        case USB_DTYPE_HID:
            if(req->wIndex == 0) {
                dev->status.data_ptr = (uint8_t*)&hid_composite_cfg_desc.keyboard.hid_desc;
                dev->status.data_count = sizeof(hid_composite_cfg_desc.keyboard.hid_desc);
            } else if(req->wIndex == 1) {
                dev->status.data_ptr = (uint8_t*)&hid_composite_cfg_desc.vendor_hid_desc;
                dev->status.data_count = sizeof(hid_composite_cfg_desc.vendor_hid_desc);
            } else {
                return usbd_fail;
            }
            return usbd_ack;
        case USB_DTYPE_HID_REPORT:
            if(req->wIndex == 0) {
                dev->status.data_ptr = (uint8_t*)hid_keyboard_report_desc;
                dev->status.data_count = sizeof(hid_keyboard_report_desc);
            } else if(req->wIndex == 1) {
                dev->status.data_ptr = (uint8_t*)hid_vendor_report_desc;
                dev->status.data_count = sizeof(hid_vendor_report_desc);
            } else {
                return usbd_fail;
            }
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }

    return usbd_fail;
}
