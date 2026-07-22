#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <furi_ble/profile_interface.h>
#include <core/common_defines.h>

/** Raw serial callback.
 * @note For data deliveries len > 0 and data points to the received bytes.
 * @note A TX-confirmation sentinel is delivered as data == NULL and len == 0.
 *       It is NOT a data packet; consumers must ignore it for relay/input parsing
 *       and use it only to pace outgoing BLE serial transmissions.
 */
typedef uint16_t (*BtRawSerialCallback)(const uint8_t* data, uint16_t len, void* context);

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_BT "bt"

typedef struct Bt Bt;

typedef enum {
    BtStatusUnavailable,
    BtStatusOff,
    BtStatusAdvertising,
    BtStatusConnected,
} BtStatus;

typedef void (*BtStatusChangedCallback)(BtStatus status, void* context);

/** Change BLE Profile
 * @note Call of this function leads to 2nd core restart
 *
 * @param bt                 Bt instance
 * @param profile_template   Profile template to change to
 * @param params             Profile parameters. Can be NULL
 *
 * @return          true on success
 */
FURI_WARN_UNUSED FuriHalBleProfileBase* bt_profile_start(
    Bt* bt,
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params);

/** Stop current BLE Profile and restore default profile
 * @note Call of this function leads to 2nd core restart
 *
 * @param bt        Bt instance
 *
 * @return          true on success
 */
bool bt_profile_restore_default(Bt* bt);

/** Disconnect from Central
 *
 * @param bt        Bt instance
 */
void bt_disconnect(Bt* bt);

/** Set callback for Bluetooth status change notification
 *
 * @param bt        Bt instance
 * @param callback  BtStatusChangedCallback instance
 * @param context   pointer to context
 */
void bt_set_status_changed_callback(Bt* bt, BtStatusChangedCallback callback, void* context);

/** Forget bonded devices
 * @note Leads to wipe ble key storage and deleting bt.keys
 *
 * @param bt        Bt instance
 */
void bt_forget_bonded_devices(Bt* bt);

/** Set keys storage file path
 *
 * @param bt                    Bt instance
 * @param keys_storage_path     Path to file with saved keys
 */
void bt_keys_storage_set_storage_path(Bt* bt, const char* keys_storage_path);

/** Set default keys storage file path
 *
 * @param bt                    Bt instance
 */
void bt_keys_storage_set_default_path(Bt* bt);

/** Set raw serial callback for AirBridge passthrough
 *
 * @param cb    callback invoked on every BLE serial RX packet
 * @param ctx   context passed to callback
 */
void bt_set_raw_serial_callback(BtRawSerialCallback cb, void* ctx);

/** Send raw bytes over BLE Serial (AirBridge TX)
 *
 * @param data  bytes to send
 * @param len   number of bytes
 * @return      true on success
 */
bool bt_serial_tx(const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif
