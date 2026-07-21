#pragma once

#include <furi_hal_usb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_VENDOR_PACKET_LEN 64

typedef enum {
    HidVendorDisconnected,
    HidVendorConnected,
    HidVendorRequest,
} HidVendorEvent;

typedef void (*HidVendorCallback)(HidVendorEvent ev, void* context);

typedef enum {
    FuriHalUsbAirbridgeProfileLogitechKbdVendor,
    FuriHalUsbAirbridgeProfileDellKbdVendor,
    FuriHalUsbAirbridgeProfileMsftKbdVendor,
    FuriHalUsbAirbridgeProfileMsftVendorOnly,
    FuriHalUsbAirbridgeProfileHpKbdVendor,
} FuriHalUsbAirbridgeProfile;

uint8_t furi_hal_usb_airbridge_profile_count(void);
FuriHalUsbInterface* furi_hal_usb_airbridge_get_profile(uint8_t index);
const char* furi_hal_usb_airbridge_profile_label(uint8_t index);
const char* furi_hal_usb_airbridge_profile_identity(uint8_t index);
bool furi_hal_usb_airbridge_profile_has_keyboard(uint8_t index);
uint16_t furi_hal_usb_airbridge_profile_vid(uint8_t index);
uint16_t furi_hal_usb_airbridge_profile_pid(uint8_t index);

/** Get HID Vendor connection state
 *
 * @return      true / false
 */
bool furi_hal_hid_vendor_is_connected(void);

/** Set HID Vendor event callback
 *
 * @param      cb  callback
 * @param      ctx  callback context
 */
void furi_hal_hid_vendor_set_callback(HidVendorCallback cb, void* ctx);

/** Get received Vendor HID packet
 *
 */
uint32_t furi_hal_hid_vendor_get_request(uint8_t* data);

/** Send Vendor HID response packet
 *
 * @param      data  response data
 * @param      len  packet length
 */
bool furi_hal_hid_vendor_send_response(uint8_t* data, uint8_t len);

/** Send a vendor response after waiting for an available interrupt IN slot. */
bool furi_hal_hid_vendor_send_response_blocking(uint8_t* data, uint8_t len, uint32_t timeout);

/** Keyboard key format matches furi_hal_hid_kb_press: HID usage plus modifiers. */
bool furi_hal_hid_airbridge_kb_press(uint16_t button);
bool furi_hal_hid_airbridge_kb_release(uint16_t button);
bool furi_hal_hid_airbridge_kb_release_all(void);

#ifdef __cplusplus
}
#endif
