#include "airbridge_config.h"

#include <stdio.h>
#include <string.h>

#include <furi_hal_usb_airbridge.h>
#include <furi_hal_version.h>

#define BLE_DEFAULT_NAME        "HP 725 K+M"
#define BLE_DEFAULT_APPEARANCE  0x0000
#define BLE_DEFAULT_MFG_COMPANY 0x0065
#define BLE_DEFAULT_DIS_MFR     "HP"
#define BLE_DEFAULT_DIS_MODEL   "HP 725 K+M"
#define BLE_DEFAULT_DIS_SERIAL  "HP5341KBD01"
#define BLE_DEFAULT_DIS_PNP     0x0126

static const AirbridgeBleIdentityParams airbridge_config_default_identity = {
    .device_name = BLE_DEFAULT_NAME,
    .mac_address = {0x3C, 0x52, 0x82, 0x00, 0x00, 0x01},
    .appearance = BLE_DEFAULT_APPEARANCE,
    .manufacturer_data =
        {
            BLE_DEFAULT_MFG_COMPANY & 0xFF,
            BLE_DEFAULT_MFG_COMPANY >> 8,
        },
    .manufacturer_data_len = 2,
    .dis_manufacturer = BLE_DEFAULT_DIS_MFR,
    .dis_model = BLE_DEFAULT_DIS_MODEL,
    .dis_serial = BLE_DEFAULT_DIS_SERIAL,
    .dis_pnp_version = BLE_DEFAULT_DIS_PNP,
};

static uint32_t airbridge_config_serial_hash(void) {
    const uint8_t* uid = furi_hal_version_uid();
    const size_t uid_size = furi_hal_version_uid_size();
    if(uid == NULL || uid_size == 0) return 0;

    uint32_t hash = 2166136261U;
    for(size_t index = 0; index < uid_size; index++) {
        hash ^= uid[index];
        hash *= 16777619U;
    }
    return hash;
}

void airbridge_config_set_defaults(AirbridgeConfig* config) {
    config->usb_profile_index = FuriHalUsbAirbridgeProfileHpKbdVendor;
    memcpy(
        &config->ble_identity,
        &airbridge_config_default_identity,
        sizeof(config->ble_identity));
    config->identity_warning = false;

    const uint32_t serial_hash = airbridge_config_serial_hash();
    if(serial_hash == 0) return;
    snprintf(
        config->ble_identity.dis_serial,
        sizeof(config->ble_identity.dis_serial),
        "HP%08lX",
        (unsigned long)serial_hash);
}
