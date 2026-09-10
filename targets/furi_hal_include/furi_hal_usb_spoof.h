#pragma once

#include <stdint.h>

#include <core/common_defines.h>
#include <furi_hal_usb.h>
#include <usb_hid.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_KBD_EP_IN          0x81
#define HID_VENDOR_EP_IN       0x82
#define HID_VENDOR_EP_OUT      0x03
#define HID_VENDOR_ONLY_EP_IN  0x81
#define HID_VENDOR_ONLY_EP_OUT 0x02
#define HID_KBD_PACKET_LEN     8
#define HID_VENDOR_PACKET_LEN  64
#define HID_INTERVAL           1

#define FURI_HAL_USB_SPOOF_VENDOR_REPORT_DESC_LEN   34
#define FURI_HAL_USB_SPOOF_KEYBOARD_REPORT_DESC_LEN 65

typedef struct HidVendorDescriptor {
    struct usb_iad_descriptor hid_iad;
    struct usb_interface_descriptor hid;
    struct usb_hid_descriptor hid_desc;
    struct usb_endpoint_descriptor hid_ep_in;
    struct usb_endpoint_descriptor hid_ep_out;
} HidVendorDescriptor;

typedef struct HidVendorConfigDescriptor {
    struct usb_config_descriptor config;
    HidVendorDescriptor vendor;
} FURI_PACKED HidVendorConfigDescriptor;

typedef struct HidKeyboardDescriptor {
    struct usb_interface_descriptor hid;
    struct usb_hid_descriptor hid_desc;
    struct usb_endpoint_descriptor hid_ep_in;
} HidKeyboardDescriptor;

typedef struct HidCompositeConfigDescriptor {
    struct usb_config_descriptor config;
    struct usb_iad_descriptor hid_iad;
    HidKeyboardDescriptor keyboard;
    struct usb_interface_descriptor vendor;
    struct usb_hid_descriptor vendor_hid_desc;
    struct usb_endpoint_descriptor vendor_ep_in;
    struct usb_endpoint_descriptor vendor_ep_out;
} FURI_PACKED HidCompositeConfigDescriptor;

typedef enum {
    FuriHalUsbSpoofProfileLogitech = 0,
    FuriHalUsbSpoofProfileDell = 1,
} FuriHalUsbSpoofProfile;

typedef struct {
    const void* device_desc;
    const void* manuf_desc;
    const void* prod_desc;
    const void* composite_config_desc;
    const void* keyboard_hid_desc;
    uint16_t keyboard_hid_desc_len;
    const void* vendor_hid_desc;
    uint16_t vendor_hid_desc_len;
} FuriHalUsbSpoofIdentity;

FuriHalUsbInterface* furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfile profile);

const FuriHalUsbSpoofIdentity*
    furi_hal_usb_spoof_get_identity(FuriHalUsbSpoofProfile profile);

const uint8_t* furi_hal_usb_spoof_vendor_report_desc(uint16_t* len);

const uint8_t* furi_hal_usb_spoof_keyboard_report_desc(uint16_t* len);

void furi_hal_usb_spoof_latch_active(FuriHalUsbSpoofProfile profile);

FuriHalUsbInterface* furi_hal_usb_spoof_get_active_interface(void);

#ifdef __cplusplus
}
#endif
