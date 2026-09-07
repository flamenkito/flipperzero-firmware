#include "bt_i.h"
#include "bt_status_registration.h"
#include <profiles/serial_profile.h>

static void bt_status_registration_lock(void* context) {
    Bt* bt = context;
    furi_check(
        furi_mutex_acquire(bt->status_callback_mutex, FuriWaitForever) == FuriStatusOk);
}

static void bt_status_registration_unlock(void* context) {
    Bt* bt = context;
    furi_check(furi_mutex_release(bt->status_callback_mutex) == FuriStatusOk);
}

static BtStatus bt_status_registration_snapshot(void* context) {
    Bt* bt = context;
    FURI_CRITICAL_ENTER();
    BtStatus status = bt->status;
    FURI_CRITICAL_EXIT();
    return status;
}

FuriHalBleProfileBase* bt_profile_start(
    Bt* bt,
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params) {
    furi_check(bt);

    // Send message
    FuriHalBleProfileBase* profile_instance = NULL;

    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeSetProfile,
        .profile_instance = &profile_instance,
        .data.profile.params = params,
        .data.profile.template = profile_template,
    };
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
    // Wait for unlock
    api_lock_wait_unlock_and_free(message.lock);

    // bt->current_profile is written only by the BtSrv thread (bt_change_profile);
    // the instance reaches the caller via BtMessage.profile_instance.
    return profile_instance;
}

bool bt_profile_restore_default(Bt* bt) {
    return bt_profile_start(bt, ble_profile_serial, NULL) != NULL;
}

bool bt_profile_restore_default_async(Bt* bt) {
    furi_check(bt);

    const BtMessage message = {
        .type = BtMessageTypeSetProfile,
        .data.profile.params = NULL,
        .data.profile.template = ble_profile_serial,
    };
    const bool queued =
        furi_message_queue_put(bt->message_queue, &message, 100U) == FuriStatusOk;
    return queued;
}

void bt_disconnect(Bt* bt) {
    furi_check(bt);

    // Send message
    BtMessage message = {.lock = api_lock_alloc_locked(), .type = BtMessageTypeDisconnect};
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);

    api_lock_wait_unlock_and_free(message.lock);
}

void bt_set_status_changed_callback(Bt* bt, BtStatusChangedCallback callback, void* context) {
    furi_check(bt_set_status_changed_callback_bounded(bt, callback, context, FuriWaitForever));
}

bool bt_set_status_changed_callback_bounded(
    Bt* bt,
    BtStatusChangedCallback callback,
    void* context,
    uint32_t timeout) {
    furi_check(bt);
    if(furi_mutex_acquire(bt->status_callback_mutex, timeout) != FuriStatusOk) return false;
    bt->status_changed_ctx = context;
    bt->status_changed_cb = callback;
    furi_check(furi_mutex_release(bt->status_callback_mutex) == FuriStatusOk);
    return true;
}

BtStatus bt_airbridge_set_status_changed_callback(
    Bt* bt,
    BtStatusChangedCallback callback,
    void* context) {
    furi_check(bt);
    const BtStatusRegistration registration = {
        .context = bt,
        .lock = bt_status_registration_lock,
        .unlock = bt_status_registration_unlock,
        .snapshot = bt_status_registration_snapshot,
        .callback_slot = &bt->status_changed_cb,
        .callback_context_slot = &bt->status_changed_ctx,
    };
    return bt_status_register_and_deliver_ordered(&registration, callback, context);
}

void bt_forget_bonded_devices(Bt* bt) {
    furi_check(bt);
    BtMessage message = {.type = BtMessageTypeForgetBondedDevices};
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
}

void bt_keys_storage_set_storage_path(Bt* bt, const char* keys_storage_path) {
    furi_check(bt);
    furi_check(bt->keys_storage);
    furi_check(keys_storage_path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(keys_storage_path);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    bt_keys_storage_set_file_path(bt->keys_storage, furi_string_get_cstr(path));

    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
}

void bt_keys_storage_set_default_path(Bt* bt) {
    furi_check(bt);
    furi_check(bt->keys_storage);

    bt_keys_storage_set_file_path(bt->keys_storage, BT_KEYS_STORAGE_PATH);
}

/*
 * Private API for the Settings app
 */

void bt_get_settings(Bt* bt, BtSettings* settings) {
    furi_assert(bt);
    furi_assert(settings);

    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeGetSettings,
        .data.settings = settings,
    };

    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);

    api_lock_wait_unlock_and_free(message.lock);
}

void bt_set_settings(Bt* bt, const BtSettings* settings) {
    furi_assert(bt);
    furi_assert(settings);

    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeSetSettings,
        .data.csettings = settings,
    };

    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);

    api_lock_wait_unlock_and_free(message.lock);
}
