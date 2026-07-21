#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BleServiceAirbridgeDevInfo BleServiceAirbridgeDevInfo;

typedef struct {
    const char* manufacturer_name;
    const char* model_number;
    const char* serial_number;
    uint16_t pnp_version;
} AirbridgeDisStrings;

BleServiceAirbridgeDevInfo*
    ble_svc_airbridge_dev_info_start(const AirbridgeDisStrings* strings);

void ble_svc_airbridge_dev_info_stop(BleServiceAirbridgeDevInfo* service);

#ifdef __cplusplus
}
#endif
