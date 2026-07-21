#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "serial_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SVC_AIRBRIDGE_SERIAL_DATA_LEN_MAX       (486)
#define BLE_SVC_AIRBRIDGE_SERIAL_CHAR_VALUE_LEN_MAX (243)

typedef SerialServiceEventCallback AirbridgeSerialServiceEventCallback;

typedef struct BleServiceAirbridgeSerial BleServiceAirbridgeSerial;

BleServiceAirbridgeSerial* ble_svc_airbridge_serial_start(void);

void ble_svc_airbridge_serial_stop(BleServiceAirbridgeSerial* service);

void ble_svc_airbridge_serial_set_callbacks(
    BleServiceAirbridgeSerial* service,
    uint16_t buff_size,
    AirbridgeSerialServiceEventCallback callback,
    void* context);

void ble_svc_airbridge_serial_set_rpc_active(BleServiceAirbridgeSerial* service, bool active);

void ble_svc_airbridge_serial_notify_buffer_is_empty(BleServiceAirbridgeSerial* service);

bool ble_svc_airbridge_serial_update_tx(
    BleServiceAirbridgeSerial* service,
    uint8_t* data,
    uint16_t data_len);

BleServiceAirbridgeSerial* ble_svc_airbridge_serial_get_active(void);

#ifdef __cplusplus
}
#endif
