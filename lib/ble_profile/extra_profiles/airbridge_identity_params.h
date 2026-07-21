#pragma once

#include <stdint.h>

/** Maximum device-name characters, excluding the terminating NUL.
 *
 * Complete BLE names of 20 characters or fewer are recommended so the name
 * can share a 31-byte scan response with manufacturer data.
 */
#define AIRBRIDGE_BLE_DEVICE_NAME_MAX_LEN (20U)

/** Size of a complete, explicit BLE public address. */
#define AIRBRIDGE_BLE_MAC_ADDRESS_LEN (6U)

/** Maximum manufacturer payload accepted by the GAP scan-response path.
 *
 * This includes the two-byte Bluetooth company identifier. The complete-name
 * and manufacturer AD structures must together fit in 31 bytes, so callers
 * must also enforce: name length + manufacturer data length <= 27.
 */
#define AIRBRIDGE_BLE_MANUFACTURER_DATA_MAX_LEN (21U)

/** Maximum DIS string characters, excluding the terminating NUL. */
#define AIRBRIDGE_BLE_DIS_MANUFACTURER_MAX_LEN (31U)
#define AIRBRIDGE_BLE_DIS_MODEL_MAX_LEN        (31U)
#define AIRBRIDGE_BLE_DIS_SERIAL_MAX_LEN       (31U)

/** Runtime BLE identity supplied by the Pocket AirBridge FAP.
 *
 * All strings are NUL-terminated. mac_address is the complete public address;
 * the FAP is responsible for validating its HP OUI before starting the profile.
 */
typedef struct {
    char device_name[AIRBRIDGE_BLE_DEVICE_NAME_MAX_LEN + 1U];
    uint8_t mac_address[AIRBRIDGE_BLE_MAC_ADDRESS_LEN];
    uint16_t appearance;

    uint8_t manufacturer_data[AIRBRIDGE_BLE_MANUFACTURER_DATA_MAX_LEN];
    uint8_t manufacturer_data_len;

    char dis_manufacturer[AIRBRIDGE_BLE_DIS_MANUFACTURER_MAX_LEN + 1U];
    char dis_model[AIRBRIDGE_BLE_DIS_MODEL_MAX_LEN + 1U];
    char dis_serial[AIRBRIDGE_BLE_DIS_SERIAL_MAX_LEN + 1U];
    uint16_t dis_pnp_version;
} AirbridgeBleIdentityParams;
