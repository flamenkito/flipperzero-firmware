#include <furi.h>
#include <furi_hal_usb.h>
#include "airbridge_usb.h"
#include <furi_hal_usb_hid.h>
#include <furi_hal_usb_spoof.h>

#include "usb.h"
#include "usb_hid.h"

/* Descriptor indices and EP0 size are part of the existing USB HAL contract;
 * its private header is not available to external applications. */
#define USB_EP0_SIZE 8
enum {
    UsbDevManuf = 1,
    UsbDevProduct = 2
};

#define HID_MOUSE_EP_IN      0x84
#define HID_MOUSE_PACKET_LEN 3

static const uint8_t mouse_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15,
    0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};

static const struct usb_hid_descriptor mouse_hid_descriptor = {
    .bLength = sizeof(struct usb_hid_descriptor),
    .bDescriptorType = USB_DTYPE_HID,
    .bcdHID = 0x0111,
    .bNumDescriptors = 1,
    .bDescriptorType0 = USB_DTYPE_HID_REPORT,
    .wDescriptorLength0 = sizeof(mouse_report_descriptor),
};

static struct {
    uint8_t bytes[128];
} mouse_configs[5];

static void airbridge_usb_append_mouse(FuriHalUsbInterface* interface, size_t index) {
    const struct usb_config_descriptor* original = interface->cfg_descr;
    const size_t length = original->wTotalLength;
    const struct usb_interface_descriptor mouse_interface = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = USB_DTYPE_INTERFACE,
        .bInterfaceNumber = original->bNumInterfaces,
        .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID,
        .bInterfaceSubClass = USB_HID_SUBCLASS_BOOT,
        .bInterfaceProtocol = USB_HID_PROTO_MOUSE,
    };
    const struct usb_endpoint_descriptor mouse_endpoint = {
        .bLength = sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType = USB_DTYPE_ENDPOINT,
        .bEndpointAddress = HID_MOUSE_EP_IN,
        .bmAttributes = USB_EPTYPE_INTERRUPT,
        .wMaxPacketSize = HID_MOUSE_PACKET_LEN,
        .bInterval = 10,
    };
    size_t total =
        length + sizeof(mouse_interface) + sizeof(mouse_hid_descriptor) + sizeof(mouse_endpoint);
    furi_check(total <= sizeof(mouse_configs[index].bytes));
    uint8_t* bytes = mouse_configs[index].bytes;
    memcpy(bytes, original, length);
    memcpy(bytes + length, &mouse_interface, sizeof(mouse_interface));
    memcpy(
        bytes + length + sizeof(mouse_interface),
        &mouse_hid_descriptor,
        sizeof(mouse_hid_descriptor));
    memcpy(bytes + total - sizeof(mouse_endpoint), &mouse_endpoint, sizeof(mouse_endpoint));
    struct usb_config_descriptor* config = (void*)bytes;
    config->wTotalLength = total;
    config->bNumInterfaces++;
    interface->cfg_descr = (void*)bytes;
}

struct HidCompositeNoIadConfigDescriptor {
    struct usb_config_descriptor config;
    HidKeyboardDescriptor keyboard;
    struct usb_interface_descriptor vendor;
    struct usb_hid_descriptor vendor_hid_desc;
    struct usb_endpoint_descriptor vendor_ep_in;
    struct usb_endpoint_descriptor vendor_ep_out;
} FURI_PACKED;

struct HidKeyboardReport {
    uint8_t mods;
    uint8_t reserved;
    uint8_t buttons[HID_KB_MAX_KEYS];
} FURI_PACKED;

static const struct usb_string_descriptor msft_manuf_desc = USB_STRING_DESC("Microsoft");
static const struct usb_string_descriptor msft_kbd_prod_desc = USB_STRING_DESC("USB Keyboard");
static const struct usb_string_descriptor msft_vendor_prod_desc =
    USB_STRING_DESC("USB Input Device");
static const struct usb_string_descriptor hp_manuf_desc = USB_STRING_DESC("PIXART");
static const struct usb_string_descriptor hp_prod_desc =
    USB_STRING_DESC("HP Wireless Keyboard and Mouse");

#define AIRBRIDGE_DEVICE_DESCRIPTOR_FULL(vid, pid, cls, sub, proto, bcd) \
    {                                                                    \
        .bLength = sizeof(struct usb_device_descriptor),                 \
        .bDescriptorType = USB_DTYPE_DEVICE,                             \
        .bcdUSB = VERSION_BCD(2, 0, 0),                                  \
        .bDeviceClass = cls,                                             \
        .bDeviceSubClass = sub,                                          \
        .bDeviceProtocol = proto,                                        \
        .bMaxPacketSize0 = USB_EP0_SIZE,                                 \
        .idVendor = vid,                                                 \
        .idProduct = pid,                                                \
        .bcdDevice = bcd,                                                \
        .iManufacturer = UsbDevManuf,                                    \
        .iProduct = UsbDevProduct,                                       \
        .iSerialNumber = 0,                                              \
        .bNumConfigurations = 1,                                         \
    }

#define AIRBRIDGE_DEVICE_DESCRIPTOR(vid, pid) \
    AIRBRIDGE_DEVICE_DESCRIPTOR_FULL(         \
        vid, pid, USB_CLASS_IAD, USB_SUBCLASS_IAD, USB_PROTO_IAD, VERSION_BCD(1, 0, 0))

static const struct usb_device_descriptor msft_kbd_device_desc =
    AIRBRIDGE_DEVICE_DESCRIPTOR(0x045E, 0x07F8);
static const struct usb_device_descriptor msft_vendor_device_desc =
    AIRBRIDGE_DEVICE_DESCRIPTOR(0x045E, 0x07A5);
static const struct usb_device_descriptor hp_device_desc =
    AIRBRIDGE_DEVICE_DESCRIPTOR_FULL(0x03F0, 0x5341, 0x00, 0x00, 0x00, VERSION_BCD(1, 2, 6));

static const struct HidVendorConfigDescriptor hid_vendor_cfg_desc = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct HidVendorConfigDescriptor),
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED | USB_CFG_ATTR_SELFPOWERED,
            .bMaxPower = USB_CFG_POWER_MA(500),
        },
    .vendor =
        {
            .hid_iad =
                {
                    .bLength = sizeof(struct usb_iad_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFASEASSOC,
                    .bFirstInterface = 0,
                    .bInterfaceCount = 1,
                    .bFunctionClass = USB_CLASS_PER_INTERFACE,
                    .bFunctionSubClass = USB_SUBCLASS_NONE,
                    .bFunctionProtocol = USB_PROTO_NONE,
                    .iFunction = NO_DESCRIPTOR,
                },
            .hid =
                {
                    .bLength = sizeof(struct usb_interface_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFACE,
                    .bInterfaceNumber = 0,
                    .bAlternateSetting = 0,
                    .bNumEndpoints = 2,
                    .bInterfaceClass = USB_CLASS_HID,
                    .bInterfaceSubClass = USB_HID_SUBCLASS_NONBOOT,
                    .bInterfaceProtocol = USB_HID_PROTO_NONBOOT,
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
                    .wDescriptorLength0 = FURI_HAL_USB_SPOOF_VENDOR_REPORT_DESC_LEN,
                },
            .hid_ep_in =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = HID_VENDOR_ONLY_EP_IN,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = HID_VENDOR_PACKET_LEN,
                    .bInterval = HID_INTERVAL,
                },
            .hid_ep_out =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = HID_VENDOR_ONLY_EP_OUT,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = HID_VENDOR_PACKET_LEN,
                    .bInterval = HID_INTERVAL,
                },
        },
};

static const struct HidCompositeNoIadConfigDescriptor hid_composite_noiad_cfg_desc = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct HidCompositeNoIadConfigDescriptor),
            .bNumInterfaces = 2,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED,
            .bMaxPower = USB_CFG_POWER_MA(100),
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
                    .wDescriptorLength0 = FURI_HAL_USB_SPOOF_KEYBOARD_REPORT_DESC_LEN,
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
            .wDescriptorLength0 = FURI_HAL_USB_SPOOF_VENDOR_REPORT_DESC_LEN,
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

static void hid_vendor_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx);
static void hid_vendor_deinit(usbd_device* dev);
static void hid_vendor_on_wakeup(usbd_device* dev);
static void hid_vendor_on_suspend(usbd_device* dev);
static usbd_respond hid_vendor_ep_config(usbd_device* dev, uint8_t cfg);
static usbd_respond
    hid_vendor_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback);

static FuriHalUsbInterface usb_airbridge = {
    .init = hid_vendor_init,
    .deinit = hid_vendor_deinit,
    .wakeup = hid_vendor_on_wakeup,
    .suspend = hid_vendor_on_suspend,
    .dev_descr = NULL,
    .str_manuf_descr = NULL,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,
    .cfg_descr = NULL,
};

static FuriHalUsbInterface usb_airbridge_dell = {
    .init = hid_vendor_init,
    .deinit = hid_vendor_deinit,
    .wakeup = hid_vendor_on_wakeup,
    .suspend = hid_vendor_on_suspend,
    .dev_descr = NULL,
    .str_manuf_descr = NULL,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,
    .cfg_descr = NULL,
};

static FuriHalUsbInterface usb_airbridge_msft_kbd = {
    .init = hid_vendor_init,
    .deinit = hid_vendor_deinit,
    .wakeup = hid_vendor_on_wakeup,
    .suspend = hid_vendor_on_suspend,
    .dev_descr = (struct usb_device_descriptor*)&msft_kbd_device_desc,
    .str_manuf_descr = (void*)&msft_manuf_desc,
    .str_prod_descr = (void*)&msft_kbd_prod_desc,
    .str_serial_descr = NULL,
    .cfg_descr = NULL,
};

static FuriHalUsbInterface usb_airbridge_msft_vendor = {
    .init = hid_vendor_init,
    .deinit = hid_vendor_deinit,
    .wakeup = hid_vendor_on_wakeup,
    .suspend = hid_vendor_on_suspend,
    .dev_descr = (struct usb_device_descriptor*)&msft_vendor_device_desc,
    .str_manuf_descr = (void*)&msft_manuf_desc,
    .str_prod_descr = (void*)&msft_vendor_prod_desc,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)&hid_vendor_cfg_desc,
};

static FuriHalUsbInterface usb_airbridge_hp = {
    .init = hid_vendor_init,
    .deinit = hid_vendor_deinit,
    .wakeup = hid_vendor_on_wakeup,
    .suspend = hid_vendor_on_suspend,
    .dev_descr = (struct usb_device_descriptor*)&hp_device_desc,
    .str_manuf_descr = (void*)&hp_manuf_desc,
    .str_prod_descr = (void*)&hp_prod_desc,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)&hid_composite_noiad_cfg_desc,
};

static const void* active_keyboard_hid_desc;
static uint16_t active_keyboard_hid_desc_len;
static const void* active_vendor_hid_desc;
static uint16_t active_vendor_hid_desc_len;

static void airbridge_usb_resolve_descriptors(void) {
    static bool resolved;
    if(resolved) return;

    const FuriHalUsbSpoofIdentity* logitech =
        furi_hal_usb_spoof_get_identity(FuriHalUsbSpoofProfileLogitech);
    const FuriHalUsbSpoofIdentity* dell =
        furi_hal_usb_spoof_get_identity(FuriHalUsbSpoofProfileDell);

    usb_airbridge.dev_descr = (struct usb_device_descriptor*)logitech->device_desc;
    usb_airbridge.str_manuf_descr = (void*)logitech->manuf_desc;
    usb_airbridge.str_prod_descr = (void*)logitech->prod_desc;
    usb_airbridge.cfg_descr = (void*)logitech->composite_config_desc;

    usb_airbridge_dell.dev_descr = (struct usb_device_descriptor*)dell->device_desc;
    usb_airbridge_dell.str_manuf_descr = (void*)dell->manuf_desc;
    usb_airbridge_dell.str_prod_descr = (void*)dell->prod_desc;
    usb_airbridge_dell.cfg_descr = (void*)dell->composite_config_desc;

    usb_airbridge_msft_kbd.cfg_descr = (void*)logitech->composite_config_desc;
    airbridge_usb_append_mouse(&usb_airbridge, 0);
    airbridge_usb_append_mouse(&usb_airbridge_dell, 1);
    airbridge_usb_append_mouse(&usb_airbridge_msft_kbd, 2);
    airbridge_usb_append_mouse(&usb_airbridge_msft_vendor, 3);
    airbridge_usb_append_mouse(&usb_airbridge_hp, 4);
    resolved = true;
}

typedef struct {
    const char* label;
    const char* identity;
    uint16_t vid;
    uint16_t pid;
    bool has_keyboard;
    FuriHalUsbInterface* interface;
} AirbridgeProfile;

static const AirbridgeProfile airbridge_profiles[] = {
    [AirbridgeUsbProfileLogitechKbdVendor] =
        {
            .label = "logitech_kbd_vendor",
            .identity = "Logitech Kbd+Vendor",
            .vid = 0x046D,
            .pid = 0xC31C,
            .has_keyboard = true,
            .interface = &usb_airbridge,
        },
    [AirbridgeUsbProfileDellKbdVendor] =
        {
            .label = "dell_kbd_vendor",
            .identity = "Dell KB216 Kbd+Vendor",
            .vid = 0x413C,
            .pid = 0x2113,
            .has_keyboard = true,
            .interface = &usb_airbridge_dell,
        },
    [AirbridgeUsbProfileMsftKbdVendor] =
        {
            .label = "msft_kbd_vendor",
            .identity = "MSFT Kbd+Vendor",
            .vid = 0x045E,
            .pid = 0x07F8,
            .has_keyboard = true,
            .interface = &usb_airbridge_msft_kbd,
        },
    [AirbridgeUsbProfileMsftVendorOnly] =
        {
            .label = "msft_vendor_only",
            .identity = "MSFT Vendor",
            .vid = 0x045E,
            .pid = 0x07A5,
            .has_keyboard = false,
            .interface = &usb_airbridge_msft_vendor,
        },
    [AirbridgeUsbProfileHpKbdVendor] =
        {
            .label = "hp_kbd_vendor",
            .identity = "HP Wireless Kbd+Mouse",
            .vid = 0x03F0,
            .pid = 0x5341,
            .has_keyboard = true,
            .interface = &usb_airbridge_hp,
        },
};

static usbd_device* usb_dev;
static FuriSemaphore* hid_vendor_semaphore;
static FuriSemaphore* hid_keyboard_semaphore;
static FuriSemaphore* hid_mouse_semaphore;
static uint8_t hid_mouse_protocol = 1;
static uint8_t hid_mouse_idle;
static bool hid_vendor_connected;
static bool hid_keyboard_available;
static uint8_t hid_vendor_ep_in = HID_VENDOR_ONLY_EP_IN;
static uint8_t hid_vendor_ep_out = HID_VENDOR_ONLY_EP_OUT;
static struct HidKeyboardReport hid_keyboard_report;
static HidVendorCallback callback;
static void* cb_ctx;

uint8_t airbridge_usb_profile_count(void) {
    return COUNT_OF(airbridge_profiles);
}

FuriHalUsbInterface* airbridge_usb_get_profile(uint8_t index) {
    return (index < COUNT_OF(airbridge_profiles)) ? airbridge_profiles[index].interface : NULL;
}

const char* airbridge_usb_profile_label(uint8_t index) {
    return (index < COUNT_OF(airbridge_profiles)) ? airbridge_profiles[index].label : NULL;
}

const char* airbridge_usb_profile_identity(uint8_t index) {
    return (index < COUNT_OF(airbridge_profiles)) ? airbridge_profiles[index].identity : NULL;
}

bool airbridge_usb_profile_has_keyboard(uint8_t index) {
    return (index < COUNT_OF(airbridge_profiles)) && airbridge_profiles[index].has_keyboard;
}

uint16_t airbridge_usb_profile_vid(uint8_t index) {
    return (index < COUNT_OF(airbridge_profiles)) ? airbridge_profiles[index].vid : 0;
}

uint16_t airbridge_usb_profile_pid(uint8_t index) {
    return (index < COUNT_OF(airbridge_profiles)) ? airbridge_profiles[index].pid : 0;
}

bool airbridge_usb_vendor_is_connected(void) {
    FURI_CRITICAL_ENTER();
    bool connected = hid_vendor_connected;
    FURI_CRITICAL_EXIT();
    return connected;
}

void airbridge_usb_vendor_set_callback(HidVendorCallback cb, void* ctx) {
    FURI_CRITICAL_ENTER();
    if((callback != NULL) && hid_vendor_connected) {
        callback(HidVendorDisconnected, cb_ctx);
    }

    if(cb) {
        cb_ctx = ctx;
        callback = cb;
    } else {
        callback = NULL;
        cb_ctx = NULL;
    }

    if((callback != NULL) && hid_vendor_connected) {
        callback(HidVendorConnected, cb_ctx);
    }
    FURI_CRITICAL_EXIT();
}

static void hid_vendor_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(ctx);
    airbridge_usb_resolve_descriptors();
    if(hid_vendor_semaphore == NULL) {
        hid_vendor_semaphore = furi_semaphore_alloc(1, 1);
    }
    if(hid_keyboard_semaphore == NULL) {
        hid_keyboard_semaphore = furi_semaphore_alloc(1, 1);
    }
    if(hid_mouse_semaphore == NULL) {
        hid_mouse_semaphore = furi_semaphore_alloc(1, 1);
    }

    usb_dev = dev;
    hid_mouse_protocol = 1;
    hid_mouse_idle = 0;
    hid_keyboard_available = intf != &usb_airbridge_msft_vendor;
    hid_vendor_ep_in = hid_keyboard_available ? HID_VENDOR_EP_IN : HID_VENDOR_ONLY_EP_IN;
    hid_vendor_ep_out = hid_keyboard_available ? HID_VENDOR_EP_OUT : HID_VENDOR_ONLY_EP_OUT;
    if(intf == &usb_airbridge_msft_vendor) {
        active_keyboard_hid_desc = NULL;
        active_keyboard_hid_desc_len = 0;
        active_vendor_hid_desc =
            &((const HidVendorConfigDescriptor*)intf->cfg_descr)->vendor.hid_desc;
        active_vendor_hid_desc_len = sizeof(hid_vendor_cfg_desc.vendor.hid_desc);
    } else if(intf == &usb_airbridge_hp) {
        active_keyboard_hid_desc =
            &((const struct HidCompositeNoIadConfigDescriptor*)intf->cfg_descr)->keyboard.hid_desc;
        active_keyboard_hid_desc_len = sizeof(hid_composite_noiad_cfg_desc.keyboard.hid_desc);
        active_vendor_hid_desc =
            &((const struct HidCompositeNoIadConfigDescriptor*)intf->cfg_descr)->vendor_hid_desc;
        active_vendor_hid_desc_len = sizeof(hid_composite_noiad_cfg_desc.vendor_hid_desc);
    } else {
        const FuriHalUsbSpoofProfile spoof_profile = intf == &usb_airbridge_dell ?
                                                         FuriHalUsbSpoofProfileDell :
                                                         FuriHalUsbSpoofProfileLogitech;
        const FuriHalUsbSpoofIdentity* identity = furi_hal_usb_spoof_get_identity(spoof_profile);
        active_keyboard_hid_desc = identity->keyboard_hid_desc;
        active_keyboard_hid_desc_len = identity->keyboard_hid_desc_len;
        active_vendor_hid_desc = identity->vendor_hid_desc;
        active_vendor_hid_desc_len = identity->vendor_hid_desc_len;
    }
    memset(&hid_keyboard_report, 0, sizeof(hid_keyboard_report));

    usbd_reg_config(dev, hid_vendor_ep_config);
    usbd_reg_control(dev, hid_vendor_control);
    usbd_connect(dev, true);
}

static void hid_vendor_deinit(usbd_device* dev) {
    hid_vendor_ep_config(dev, 0);
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
    FURI_CRITICAL_ENTER();
    hid_vendor_connected = false;
    FURI_CRITICAL_EXIT();
    usb_dev = NULL;
}

void airbridge_usb_free(void) {
    /* UsbSrv may reinitialize an active interface while a sender is waiting.
     * Keep semaphores across reinit; free only after synchronous restoration. */
    furi_check(usb_dev == NULL);
    if(hid_vendor_semaphore) furi_semaphore_free(hid_vendor_semaphore);
    if(hid_keyboard_semaphore) furi_semaphore_free(hid_keyboard_semaphore);
    if(hid_mouse_semaphore) furi_semaphore_free(hid_mouse_semaphore);
    hid_vendor_semaphore = NULL;
    hid_keyboard_semaphore = NULL;
    hid_mouse_semaphore = NULL;
}

static void hid_vendor_on_wakeup(usbd_device* dev) {
    UNUSED(dev);
    FURI_CRITICAL_ENTER();
    hid_vendor_connected = true;
    if(callback != NULL) {
        callback(HidVendorConnected, cb_ctx);
    }
    FURI_CRITICAL_EXIT();
}

static void hid_vendor_on_suspend(usbd_device* dev) {
    UNUSED(dev);
    FURI_CRITICAL_ENTER();
    if(hid_vendor_connected) {
        hid_vendor_connected = false;
        furi_semaphore_release(hid_vendor_semaphore);
        furi_semaphore_release(hid_keyboard_semaphore);
        furi_semaphore_release(hid_mouse_semaphore);
        if(callback != NULL) {
            callback(HidVendorDisconnected, cb_ctx);
        }
    }
    FURI_CRITICAL_EXIT();
}

bool airbridge_usb_vendor_send_response(uint8_t* data, uint8_t len) {
    return airbridge_usb_vendor_send_response_blocking(data, len, 0);
}

bool airbridge_usb_vendor_send_response_blocking(uint8_t* data, uint8_t len, uint32_t timeout) {
    if((hid_vendor_semaphore == NULL) || (len > HID_VENDOR_PACKET_LEN)) return false;
    if(furi_semaphore_acquire(hid_vendor_semaphore, timeout) != FuriStatusOk) return false;
    if((usb_dev == NULL) || !hid_vendor_connected) {
        furi_semaphore_release(hid_vendor_semaphore);
        return false;
    }
    if(usbd_ep_write(usb_dev, hid_vendor_ep_in, data, len) < 0) {
        furi_semaphore_release(hid_vendor_semaphore);
        return false;
    }
    return true;
}

uint32_t airbridge_usb_vendor_get_request(uint8_t* data) {
    if(usb_dev == NULL) return 0;
    int32_t len = usbd_ep_read(usb_dev, hid_vendor_ep_out, data, HID_VENDOR_PACKET_LEN);
    return (len < 0) ? 0 : len;
}

bool airbridge_usb_mouse_move(int8_t x, int8_t y) {
    if(!hid_mouse_semaphore || !usb_dev || !hid_vendor_connected) return false;
    if(furi_semaphore_acquire(hid_mouse_semaphore, 0) != FuriStatusOk) return false;
    const uint8_t report[HID_MOUSE_PACKET_LEN] = {0, (uint8_t)x, (uint8_t)y};
    if(!usb_dev || !hid_vendor_connected ||
       usbd_ep_write(usb_dev, HID_MOUSE_EP_IN, report, sizeof(report)) < 0) {
        furi_semaphore_release(hid_mouse_semaphore);
        return false;
    }
    return true;
}

bool airbridge_usb_kb_press(uint16_t button) {
    if((hid_keyboard_semaphore == NULL) || !hid_keyboard_available || !hid_vendor_connected) {
        return false;
    }
    if(furi_semaphore_acquire(hid_keyboard_semaphore, HID_INTERVAL * 2) != FuriStatusOk) {
        return false;
    }
    hid_keyboard_report.mods |= button >> 8;
    hid_keyboard_report.buttons[0] = button & 0xFF;
    if(usbd_ep_write(usb_dev, HID_KBD_EP_IN, &hid_keyboard_report, sizeof(hid_keyboard_report)) <
       0) {
        furi_semaphore_release(hid_keyboard_semaphore);
        return false;
    }
    return true;
}

bool airbridge_usb_kb_release(uint16_t button) {
    if((hid_keyboard_semaphore == NULL) || !hid_keyboard_available || !hid_vendor_connected) {
        return false;
    }
    if(furi_semaphore_acquire(hid_keyboard_semaphore, HID_INTERVAL * 2) != FuriStatusOk) {
        return false;
    }
    hid_keyboard_report.mods &= ~(button >> 8);
    if(hid_keyboard_report.buttons[0] == (button & 0xFF)) {
        hid_keyboard_report.buttons[0] = 0;
    }
    if(usbd_ep_write(usb_dev, HID_KBD_EP_IN, &hid_keyboard_report, sizeof(hid_keyboard_report)) <
       0) {
        furi_semaphore_release(hid_keyboard_semaphore);
        return false;
    }
    return true;
}

bool airbridge_usb_kb_release_all(void) {
    if((hid_keyboard_semaphore == NULL) || !hid_keyboard_available || !hid_vendor_connected) {
        return false;
    }
    if(furi_semaphore_acquire(hid_keyboard_semaphore, HID_INTERVAL * 2) != FuriStatusOk) {
        return false;
    }
    memset(&hid_keyboard_report, 0, sizeof(hid_keyboard_report));
    if(usbd_ep_write(usb_dev, HID_KBD_EP_IN, &hid_keyboard_report, sizeof(hid_keyboard_report)) <
       0) {
        furi_semaphore_release(hid_keyboard_semaphore);
        return false;
    }
    return true;
}

static void hid_vendor_rx_ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);
    UNUSED(event);
    UNUSED(ep);
    FURI_CRITICAL_ENTER();
    if(callback != NULL) {
        callback(HidVendorRequest, cb_ctx);
    }
    FURI_CRITICAL_EXIT();
}

static void hid_vendor_txrx_ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);
    if((event == usbd_evt_eptx) && (ep == hid_vendor_ep_in)) {
        furi_semaphore_release(hid_vendor_semaphore);
    } else if((event == usbd_evt_eptx) && hid_keyboard_available && (ep == HID_KBD_EP_IN)) {
        furi_semaphore_release(hid_keyboard_semaphore);
    } else if((event == usbd_evt_eptx) && (ep == HID_MOUSE_EP_IN)) {
        furi_semaphore_release(hid_mouse_semaphore);
    } else if(ep == hid_vendor_ep_out) {
        hid_vendor_rx_ep_callback(dev, event, ep);
    }
}

static usbd_respond hid_vendor_ep_config(usbd_device* dev, uint8_t cfg) {
    switch(cfg) {
    case 0:
        usbd_ep_deconfig(dev, HID_MOUSE_EP_IN);
        usbd_reg_endpoint(dev, HID_MOUSE_EP_IN, 0);
        usbd_ep_deconfig(dev, hid_vendor_ep_out);
        usbd_ep_deconfig(dev, hid_vendor_ep_in);
        usbd_reg_endpoint(dev, hid_vendor_ep_out, 0);
        usbd_reg_endpoint(dev, hid_vendor_ep_in, 0);
        if(hid_keyboard_available) {
            usbd_ep_deconfig(dev, HID_KBD_EP_IN);
            usbd_reg_endpoint(dev, HID_KBD_EP_IN, 0);
        }
        return usbd_ack;
    case 1:
        usbd_ep_config(dev, HID_MOUSE_EP_IN, USB_EPTYPE_INTERRUPT, HID_MOUSE_PACKET_LEN);
        usbd_reg_endpoint(dev, HID_MOUSE_EP_IN, hid_vendor_txrx_ep_callback);
        furi_semaphore_release(hid_mouse_semaphore);
        if(hid_keyboard_available) {
            usbd_ep_config(dev, HID_KBD_EP_IN, USB_EPTYPE_INTERRUPT, HID_KBD_PACKET_LEN);
            usbd_reg_endpoint(dev, HID_KBD_EP_IN, hid_vendor_txrx_ep_callback);
        }
        usbd_ep_config(dev, hid_vendor_ep_in, USB_EPTYPE_INTERRUPT, HID_VENDOR_PACKET_LEN);
        usbd_ep_config(dev, hid_vendor_ep_out, USB_EPTYPE_INTERRUPT, HID_VENDOR_PACKET_LEN);
        usbd_reg_endpoint(dev, hid_vendor_ep_in, hid_vendor_txrx_ep_callback);
        usbd_reg_endpoint(dev, hid_vendor_ep_out, hid_vendor_txrx_ep_callback);
        usbd_ep_write(dev, hid_vendor_ep_in, 0, 0);
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

static usbd_respond
    hid_vendor_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* control_callback) {
    UNUSED(control_callback);
    const uint16_t mouse_interface = hid_keyboard_available ? 2 : 1;
    if(req->wIndex == mouse_interface &&
       (req->bmRequestType & USB_REQ_RECIPIENT) == USB_REQ_INTERFACE) {
        if((req->bmRequestType & USB_REQ_TYPE) == USB_REQ_CLASS) {
            if(req->bRequest == USB_HID_SETIDLE) {
                hid_mouse_idle = req->wValue >> 8;
                return usbd_ack;
            }
            if(req->bRequest == USB_HID_SETPROTOCOL && req->wValue <= 1) {
                hid_mouse_protocol = req->wValue;
                return usbd_ack;
            }
            if(req->bRequest == USB_HID_GETPROTOCOL || req->bRequest == USB_HID_GETIDLE) {
                dev->status.data_ptr = req->bRequest == USB_HID_GETPROTOCOL ? &hid_mouse_protocol :
                                                                              &hid_mouse_idle;
                dev->status.data_count = 1;
                return usbd_ack;
            }
            if(req->bRequest == USB_HID_GETREPORT) {
                static const uint8_t released[HID_MOUSE_PACKET_LEN] = {0};
                dev->status.data_ptr = (uint8_t*)released;
                dev->status.data_count = sizeof(released);
                return usbd_ack;
            }
        } else if(
            (req->bmRequestType & USB_REQ_TYPE) == USB_REQ_STANDARD &&
            req->bRequest == USB_STD_GET_DESCRIPTOR) {
            if((req->wValue >> 8) == USB_DTYPE_HID) {
                dev->status.data_ptr = (uint8_t*)&mouse_hid_descriptor;
                dev->status.data_count = sizeof(mouse_hid_descriptor);
                return usbd_ack;
            }
            if((req->wValue >> 8) == USB_DTYPE_HID_REPORT) {
                dev->status.data_ptr = (uint8_t*)mouse_report_descriptor;
                dev->status.data_count = sizeof(mouse_report_descriptor);
                return usbd_ack;
            }
        }
        return usbd_fail;
    }
    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_CLASS) &&
       ((req->wIndex == 0) || (hid_keyboard_available && (req->wIndex == 1)))) {
        switch(req->bRequest) {
        case USB_HID_SETIDLE:
            return usbd_ack;
        case USB_HID_SETPROTOCOL:
            return (hid_keyboard_available && (req->wIndex == 0)) ? usbd_ack : usbd_fail;
        case USB_HID_GETREPORT:
            if(hid_keyboard_available && (req->wIndex == 0)) {
                dev->status.data_ptr = (uint8_t*)&hid_keyboard_report;
                dev->status.data_count = sizeof(hid_keyboard_report);
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
            if(hid_keyboard_available && (req->wIndex == 0)) {
                dev->status.data_ptr = (uint8_t*)active_keyboard_hid_desc;
                dev->status.data_count = active_keyboard_hid_desc_len;
            } else if(
                (hid_keyboard_available && (req->wIndex == 1)) ||
                (!hid_keyboard_available && (req->wIndex == 0))) {
                dev->status.data_ptr = (uint8_t*)active_vendor_hid_desc;
                dev->status.data_count = active_vendor_hid_desc_len;
            } else {
                return usbd_fail;
            }
            return usbd_ack;
        case USB_DTYPE_HID_REPORT:
            if(hid_keyboard_available && (req->wIndex == 0)) {
                uint16_t report_len = 0;
                dev->status.data_ptr =
                    (uint8_t*)furi_hal_usb_spoof_keyboard_report_desc(&report_len);
                dev->status.data_count = report_len;
            } else if(
                (hid_keyboard_available && (req->wIndex == 1)) ||
                (!hid_keyboard_available && (req->wIndex == 0))) {
                uint16_t report_len = 0;
                dev->status.data_ptr =
                    (uint8_t*)furi_hal_usb_spoof_vendor_report_desc(&report_len);
                dev->status.data_count = report_len;
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
