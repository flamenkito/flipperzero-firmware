#pragma once

/* App-owned USB transport. No AirBridge USB symbols are exported by firmware. */

#include <furi_hal_usb.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call only after restoring the previous USB interface and joining senders. */
void airbridge_usb_free(void);

#define HID_VENDOR_PACKET_LEN 64

typedef enum {
    HidVendorDisconnected,
    HidVendorConnected,
    HidVendorRequest,
} HidVendorEvent;

typedef void (*HidVendorCallback)(HidVendorEvent ev, void* context);

typedef enum {
    AirbridgeUsbProfileLogitechKbdVendor,
    AirbridgeUsbProfileDellKbdVendor,
    AirbridgeUsbProfileMsftKbdVendor,
    AirbridgeUsbProfileMsftVendorOnly,
    AirbridgeUsbProfileHpKbdVendor,
} AirbridgeUsbProfile;

uint8_t airbridge_usb_profile_count(void);
FuriHalUsbInterface* airbridge_usb_get_profile(uint8_t index);
const char* airbridge_usb_profile_label(uint8_t index);
const char* airbridge_usb_profile_identity(uint8_t index);
bool airbridge_usb_profile_has_keyboard(uint8_t index);
uint16_t airbridge_usb_profile_vid(uint8_t index);
uint16_t airbridge_usb_profile_pid(uint8_t index);

/** Get HID Vendor connection state
 *
 * @return      true / false
 */
bool airbridge_usb_vendor_is_connected(void);

/** Set HID Vendor event callback
 *
 * @param      cb  callback
 * @param      ctx  callback context
 */
void airbridge_usb_vendor_set_callback(HidVendorCallback cb, void* ctx);

/** Get received Vendor HID packet
 *
 */
uint32_t airbridge_usb_vendor_get_request(uint8_t* data);

/** Send Vendor HID response packet
 *
 * @param      data  response data
 * @param      len  packet length
 */
bool airbridge_usb_vendor_send_response(uint8_t* data, uint8_t len);

/** Send a vendor response after waiting for an available interrupt IN slot. */
bool airbridge_usb_vendor_send_response_blocking(uint8_t* data, uint8_t len, uint32_t timeout);

/** Keyboard key format matches furi_hal_hid_kb_press: HID usage plus modifiers. */
bool airbridge_usb_kb_press(uint16_t button);
bool airbridge_usb_kb_release(uint16_t button);
bool airbridge_usb_kb_release_all(void);
bool airbridge_usb_mouse_move(int8_t x, int8_t y);

#ifdef __cplusplus
}
#endif
