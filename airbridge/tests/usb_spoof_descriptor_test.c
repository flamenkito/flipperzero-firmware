#include "platform/furi.h"

#include <furi_hal_rtc.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_usb_spoof.h>

/* Mechanical legacy fixture copied from
 * git show HEAD:applications_user/pocket_airbridge/airbridge_usb.c. */
#define HID_PAGE_VENDOR_FIXTURE   0xFF00
#define HID_VENDOR_USAGE_FIXTURE  0x01
#define HID_VENDOR_INPUT_FIXTURE  0x20
#define HID_VENDOR_OUTPUT_FIXTURE 0x21

static const uint8_t legacy_vendor_report_desc[] = {
    HID_RI_USAGE_PAGE(16, HID_PAGE_VENDOR_FIXTURE),
    HID_USAGE(HID_VENDOR_USAGE_FIXTURE),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_USAGE(HID_VENDOR_INPUT_FIXTURE),
    HID_LOGICAL_MINIMUM(0x00),
    HID_RI_LOGICAL_MAXIMUM(16, 0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(HID_VENDOR_PACKET_LEN),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_USAGE(HID_VENDOR_OUTPUT_FIXTURE),
    HID_LOGICAL_MINIMUM(0x00),
    HID_RI_LOGICAL_MAXIMUM(16, 0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(HID_VENDOR_PACKET_LEN),
    HID_OUTPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
};

static const uint8_t legacy_keyboard_report_desc[] = {
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

static const struct usb_string_descriptor legacy_logitech_manuf = USB_STRING_DESC("Logitech");
static const struct usb_string_descriptor legacy_logitech_prod = USB_STRING_DESC("USB Keyboard");
static const struct usb_string_descriptor legacy_dell_manuf = USB_STRING_DESC("Dell");
static const struct usb_string_descriptor legacy_dell_prod = USB_STRING_DESC("KB216 Keyboard");

#define LEGACY_DEVICE_DESCRIPTOR(vid, pid)               \
    {                                                     \
        .bLength = sizeof(struct usb_device_descriptor),  \
        .bDescriptorType = USB_DTYPE_DEVICE,              \
        .bcdUSB = VERSION_BCD(2, 0, 0),                   \
        .bDeviceClass = USB_CLASS_IAD,                    \
        .bDeviceSubClass = USB_SUBCLASS_IAD,              \
        .bDeviceProtocol = USB_PROTO_IAD,                 \
        .bMaxPacketSize0 = 8,                             \
        .idVendor = vid,                                  \
        .idProduct = pid,                                 \
        .bcdDevice = VERSION_BCD(1, 0, 0),                \
        .iManufacturer = 1,                               \
        .iProduct = 2,                                    \
        .iSerialNumber = 0,                               \
        .bNumConfigurations = 1,                          \
    }

static const struct usb_device_descriptor legacy_logitech_device =
    LEGACY_DEVICE_DESCRIPTOR(0x046D, 0xC31C);
static const struct usb_device_descriptor legacy_dell_device =
    LEGACY_DEVICE_DESCRIPTOR(0x413C, 0x2113);

static const HidCompositeConfigDescriptor legacy_composite = {
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
                    .wDescriptorLength0 = sizeof(legacy_keyboard_report_desc),
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
            .wDescriptorLength0 = sizeof(legacy_vendor_report_desc),
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

FuriHalUsbIdentity furi_hal_rtc_get_usb_identity(void) {
    return FuriHalUsbIdentityLogitech;
}

static void assert_bytes(const void* actual, const void* expected, size_t size) {
    assert(actual != NULL);
    assert(memcmp(actual, expected, size) == 0);
    UNUSED(actual);
    UNUSED(expected);
    UNUSED(size);
}

static void assert_string(const void* actual, const struct usb_string_descriptor* expected) {
    assert_bytes(actual, expected, expected->bLength);
}

static void assert_identity(
    FuriHalUsbSpoofProfile profile,
    const struct usb_device_descriptor* device,
    const struct usb_string_descriptor* manufacturer,
    const struct usb_string_descriptor* product) {
    const FuriHalUsbSpoofIdentity* identity = furi_hal_usb_spoof_get_identity(profile);
    FuriHalUsbInterface* interface = furi_hal_usb_spoof_get_interface(profile);
    assert(interface != NULL);
    assert(interface->dev_descr == identity->device_desc);
    assert(interface->str_manuf_descr == identity->manuf_desc);
    assert(interface->str_prod_descr == identity->prod_desc);
    assert(interface->cfg_descr == identity->composite_config_desc);
    UNUSED(interface);
    assert_bytes(identity->device_desc, device, sizeof(*device));
    assert_string(identity->manuf_desc, manufacturer);
    assert_string(identity->prod_desc, product);
    assert_bytes(identity->composite_config_desc, &legacy_composite, sizeof(legacy_composite));
    assert_bytes(
        identity->keyboard_hid_desc,
        &legacy_composite.keyboard.hid_desc,
        identity->keyboard_hid_desc_len);
    assert(identity->keyboard_hid_desc_len == sizeof(legacy_composite.keyboard.hid_desc));
    assert_bytes(
        identity->vendor_hid_desc,
        &legacy_composite.vendor_hid_desc,
        identity->vendor_hid_desc_len);
    assert(identity->vendor_hid_desc_len == sizeof(legacy_composite.vendor_hid_desc));
}

int main(void) {
    _Static_assert(
        sizeof(legacy_vendor_report_desc) == FURI_HAL_USB_SPOOF_VENDOR_REPORT_DESC_LEN,
        "legacy vendor report fixture length");
    _Static_assert(
        sizeof(legacy_keyboard_report_desc) == FURI_HAL_USB_SPOOF_KEYBOARD_REPORT_DESC_LEN,
        "legacy keyboard report fixture length");
    _Static_assert(sizeof(legacy_composite) == 74, "legacy composite fixture length");

    uint16_t length = 0;
    assert_bytes(
        furi_hal_usb_spoof_vendor_report_desc(&length),
        legacy_vendor_report_desc,
        sizeof(legacy_vendor_report_desc));
    assert(length == sizeof(legacy_vendor_report_desc));
    assert_bytes(
        furi_hal_usb_spoof_keyboard_report_desc(&length),
        legacy_keyboard_report_desc,
        sizeof(legacy_keyboard_report_desc));
    assert(length == sizeof(legacy_keyboard_report_desc));

    assert_identity(
        FuriHalUsbSpoofProfileLogitech,
        &legacy_logitech_device,
        &legacy_logitech_manuf,
        &legacy_logitech_prod);
    assert_identity(
        FuriHalUsbSpoofProfileDell,
        &legacy_dell_device,
        &legacy_dell_manuf,
        &legacy_dell_prod);
    assert(
        furi_hal_usb_spoof_get_interface((FuriHalUsbSpoofProfile)15) ==
        furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileLogitech));
    furi_hal_usb_spoof_latch_active(FuriHalUsbSpoofProfileDell);
    assert(
        furi_hal_usb_spoof_get_active_interface() ==
        furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileDell));
    return 0;
}
