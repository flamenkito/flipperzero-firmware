#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <furi_ble/profile_interface.h>
#include <core/common_defines.h>

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

/** True while pairing awaits user input (shown PIN or numeric comparison).
 * Written on the GAP event thread; cleared after comparison or link completion.
 *
 * @param bt        Bt instance
 *
 * @return          true if pairing is in progress, false otherwise
 */
bool bt_pairing_in_progress(Bt* bt);

/** Borrow the current profile across direct service operations.
 * Returns NULL if no profile is available. Every non-NULL result must be
 * released. Do not hold a reference across bt_profile_start, bt_disconnect,
 * UI waits, or other Bt service queue operations. Profile replacement waits
 * for references to drain before running the profile destructor.
 */
FuriHalBleProfileBase* bt_current_profile_acquire(Bt* bt);
void bt_current_profile_release(Bt* bt);

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

/** Register a status callback and deliver a race-free status snapshot.
 *
 * The initial callback runs outside the callback-state mutex but inside the
 * recursive dispatch mutex, totally ordering it before later deliveries.
 *
 * @param bt        Bt instance
 * @param callback  BtStatusChangedCallback instance
 * @param context   pointer to context
 * @return          status delivered during registration
 */
BtStatus bt_set_status_changed_callback_with_snapshot(
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

#ifdef __cplusplus
}
#endif
