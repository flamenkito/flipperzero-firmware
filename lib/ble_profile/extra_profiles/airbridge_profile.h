#pragma once

#include "airbridge_identity_params.h"

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pocket AirBridge composite BLE profile descriptor. */
extern const FuriHalBleProfileTemplate* const ble_profile_airbridge;

/** Send a keyboard input report (HID report ID 1). */
bool ble_profile_airbridge_kb_report(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t len);

/** Send a mouse input report (HID report ID 2). */
bool ble_profile_airbridge_mouse_report(
    FuriHalBleProfileBase* profile,
    uint8_t* data,
    uint16_t len);

/** Send a consumer-control input report (HID report ID 3). */
bool ble_profile_airbridge_consumer_report(
    FuriHalBleProfileBase* profile,
    uint8_t* data,
    uint16_t len);

#ifdef __cplusplus
}
#endif
