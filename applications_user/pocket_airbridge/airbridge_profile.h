#pragma once

#include "airbridge_identity_params.h"
#include "airbridge_serial_service.h"

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

/** App-owned serial-only BLE profile. */
extern const FuriHalBleProfileTemplate* const ble_profile_airbridge;

typedef struct {
    AirbridgeBleIdentityParams identity;
    AirbridgeSerialServiceEventCallback callback;
    void* context;
} AirbridgeBleProfileParams;

bool airbridge_profile_send(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t len);
bool airbridge_profile_subscribed(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
