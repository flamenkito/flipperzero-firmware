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

/** Queue restoration of the default profile without waiting for radio teardown.
 * The request owns no caller memory and is safe to complete after the caller exits.
 *
 * @param bt        Bt instance
 *
 * @return          true if the request was queued
 */
FURI_WARN_UNUSED bool bt_profile_restore_default_async(Bt* bt);

/** Disconnect from Central
 *
 * @param bt        Bt instance
 */
void bt_disconnect(Bt* bt);

/** True while a pairing ceremony (PIN show/numeric comparison) is in
 * progress on the current link. Written on the GAP event thread.
 *
 * @param bt        Bt instance
 *
 * @return          true if pairing is in progress, false otherwise
 */
bool bt_pairing_in_progress(Bt* bt);

/** Send an AirBridge keyboard input report through the current BLE profile.
 *
 * The Bt service holds a reader reference for the entire report operation, so
 * profile replacement cannot free the profile while the report is in flight.
 *
 * @param bt        Bt instance
 * @param data      eight-byte keyboard report
 * @param len       report length
 * @return          false on success, true on error
 */
bool bt_airbridge_kb_report(Bt* bt, uint8_t* data, uint16_t len);

/** Query whether the current AirBridge serial client subscribed to TX.
 *
 * The service/profile pair remains reader-referenced for the whole query.
 *
 * @param bt        Bt instance
 * @return          true only for a live AirBridge profile with a subscribed client
 */
bool bt_airbridge_serial_client_subscribed(Bt* bt);

/** Set callback for Bluetooth status change notification
 *
 * Callback invocation is serialized with registration changes. The bounded
 * variant leaves the existing registration unchanged when its timeout expires.
 * Unregistering from inside the callback is supported.
 *
 * @param bt        Bt instance
 * @param callback  BtStatusChangedCallback instance
 * @param context   pointer to context
 */
void bt_set_status_changed_callback(Bt* bt, BtStatusChangedCallback callback, void* context);
bool bt_set_status_changed_callback_bounded(
    Bt* bt,
    BtStatusChangedCallback callback,
    void* context,
    uint32_t timeout);

/** Register the AirBridge status callback and deliver a race-free status snapshot.
 *
 * The initial callback runs outside the callback-state mutex but inside the
 * recursive dispatch mutex, totally ordering it before later deliveries.
 *
 * @param bt        Bt instance
 * @param callback  BtStatusChangedCallback instance
 * @param context   pointer to context
 * @return          status delivered during registration
 */
BtStatus bt_airbridge_set_status_changed_callback(
    Bt* bt,
    BtStatusChangedCallback callback,
    void* context);

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
