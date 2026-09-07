#include "bt_i.h"
#include "bt_keys_storage.h"
#include "bt_profile_quiescence.h"

#include <core/check.h>
#include <furi_hal_bt.h>
#include <services/battery_service.h>
#include <notification/notification_messages.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <profiles/serial_profile.h>
#include <extra_profiles/airbridge_profile.h>
#include <services/airbridge_serial_service.h>

#define TAG "BtSrv"

static void bt_statusbar_update(Bt* bt);

#define BT_RPC_EVENT_BUFF_SENT    (1UL << 0)
#define BT_RPC_EVENT_DISCONNECTED (1UL << 1)
#define BT_RPC_EVENT_ALL          (BT_RPC_EVENT_BUFF_SENT | BT_RPC_EVENT_DISCONNECTED)

#define ICON_SPACER                      2
#define BT_PROFILE_RETRY_DELAY_MS        (250U)
#define BT_PROFILE_QUIESCENCE_TIMEOUT_MS (1000U)
#define BT_PROFILE_MUTEX_TIMEOUT_MS      (50U)
#define BT_PRODUCER_QUEUE_TIMEOUT_MS     (100U)

/* Pocket AirBridge raw serial passthrough hook */
static BtRawSerialCallback bt_raw_serial_cb = NULL;
static void* bt_raw_serial_ctx = NULL;
static Bt* bt_instance = NULL;

static bool bt_profile_is_airbridge(FuriHalBleProfileBase* profile) {
    return profile && furi_hal_bt_check_profile_type(profile, ble_profile_airbridge);
}

/* Publish a new current_profile and refresh the cached type flags.
 * Always called with bt->current_profile_mutex held, only from the BtSrv thread. */
static void bt_publish_current_profile(Bt* bt, FuriHalBleProfileBase* profile) {
    bt->current_profile = profile;
    bt->current_profile_is_serial = furi_hal_bt_check_profile_type(profile, ble_profile_serial);
    bt->current_profile_is_airbridge = bt_profile_is_airbridge(profile);
}

static bool bt_publish_current_profile_bounded(Bt* bt, FuriHalBleProfileBase* profile) {
    if(furi_mutex_acquire(bt->current_profile_mutex, BT_PROFILE_MUTEX_TIMEOUT_MS) !=
       FuriStatusOk) {
        return false;
    }
    bt_publish_current_profile(bt, profile);
    furi_check(furi_mutex_release(bt->current_profile_mutex) == FuriStatusOk);
    return true;
}

static bool
    bt_unpublish_current_profile_bounded(Bt* bt, FuriHalBleProfileBase** previous_profile) {
    if(furi_mutex_acquire(bt->current_profile_mutex, BT_PROFILE_MUTEX_TIMEOUT_MS) !=
       FuriStatusOk) {
        return false;
    }
    *previous_profile = bt->current_profile;
    bt_publish_current_profile(bt, NULL);
    furi_check(furi_mutex_release(bt->current_profile_mutex) == FuriStatusOk);
    return true;
}

static bool bt_current_profile_is_null_bounded(Bt* bt, bool* is_null) {
    if(furi_mutex_acquire(bt->current_profile_mutex, BT_PROFILE_MUTEX_TIMEOUT_MS) !=
       FuriStatusOk) {
        return false;
    }
    *is_null = bt->current_profile == NULL;
    furi_check(furi_mutex_release(bt->current_profile_mutex) == FuriStatusOk);
    return true;
}

/* Reader-reference protocol for blocking current_profile users.
 *
 * Invariant: current_profile_mutex is only ever held for pointer/counter
 * manipulation (microseconds) - never across hci_send_req/aci/event waits.
 * hci_send_req blocks on hci_sem, which is released only by the BleEventWorker
 * thread; that thread takes the mutex (briefly) in bt_on_gap_event_callback,
 * so holding the mutex across an aci call is a circular wait (confirmed
 * hardware wedge). A reader reference instead keeps the profile and its
 * serial service alive across the blocking call: the writer publishes NULL
 * first (so no new readers can start), waits for quiescence, and only then
 * lets furi_hal_bt_change_app free the old profile. */
static FuriHalBleProfileBase* bt_current_profile_acquire(Bt* bt) {
    if(furi_mutex_acquire(bt->current_profile_mutex, BT_PROFILE_MUTEX_TIMEOUT_MS) !=
       FuriStatusOk) {
        return NULL;
    }
    FuriHalBleProfileBase* profile = bt->current_profile;
    if(profile) {
        bt->current_profile_readers++;
    }
    furi_mutex_release(bt->current_profile_mutex);
    return profile;
}

static void bt_current_profile_release(Bt* bt) {
    furi_check(
        furi_mutex_acquire(bt->current_profile_mutex, BT_PROFILE_MUTEX_TIMEOUT_MS) ==
        FuriStatusOk);
    furi_assert(bt->current_profile_readers > 0);
    bt->current_profile_readers--;
    furi_mutex_release(bt->current_profile_mutex);
}

/* Writer-side drain: called AFTER publishing NULL (no new readers can start)
 * and BEFORE furi_hal_bt_change_app frees the old profile. Polls the reader
 * counter until it reaches zero. The bound is the longest reader-critical
 * section, which may include blocking aci_* calls, FuriWaitForever message
 * queue puts, and the PIN-verify modal dialog (bt_on_gap_event_callback holds
 * its ref across all of these). The mutex is held only for the pointer/counter
 * snapshot (microseconds) and released before furi_delay_ms(1). */
static bool bt_current_profile_readers_snapshot(void* context, uint32_t* readers) {
    Bt* bt = context;
    if(furi_mutex_acquire(bt->current_profile_mutex, BT_PROFILE_MUTEX_TIMEOUT_MS) !=
       FuriStatusOk) {
        return false;
    }
    *readers = bt->current_profile_readers;
    furi_mutex_release(bt->current_profile_mutex);
    return true;
}

static void bt_current_profile_wait_step(void* context) {
    UNUSED(context);
    furi_delay_ms(1);
}

static bool bt_current_profile_wait_quiescent(Bt* bt) {
    const BtProfileQuiescenceOps ops = {
        .context = bt,
        .readers_snapshot = bt_current_profile_readers_snapshot,
        .wait_one_step = bt_current_profile_wait_step,
    };
    return bt_profile_wait_quiescent_bounded(&ops, BT_PROFILE_QUIESCENCE_TIMEOUT_MS);
}

void bt_set_raw_serial_callback(BtRawSerialCallback cb, void* ctx) {
    FURI_CRITICAL_ENTER();
    if(cb) {
        bt_raw_serial_ctx = ctx;
        bt_raw_serial_cb = cb;
    } else {
        bt_raw_serial_cb = NULL;
        bt_raw_serial_ctx = NULL;
    }
    FURI_CRITICAL_EXIT();
}

static bool bt_raw_serial_invoke(const uint8_t* data, uint16_t size, uint16_t* result) {
    bool invoked = false;
    FURI_CRITICAL_ENTER();
    if(bt_raw_serial_cb) {
        *result = bt_raw_serial_cb(data, size, bt_raw_serial_ctx);
        invoked = true;
    }
    FURI_CRITICAL_EXIT();
    return invoked;
}

bool bt_pairing_in_progress(Bt* bt) {
    furi_check(bt);
    return bt->pairing_in_progress;
}

bool bt_airbridge_kb_report(Bt* bt, uint8_t* data, uint16_t len) {
    if(!bt) return true;

    bool error = true;
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
    if(bt_profile_is_airbridge(profile)) {
        error = ble_profile_airbridge_kb_report(profile, data, len);
    }
    if(profile) {
        bt_current_profile_release(bt);
    }
    return error;
}

bool bt_airbridge_serial_client_subscribed(Bt* bt) {
    if(!bt) return false;

    bool subscribed = false;
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
    if(bt_profile_is_airbridge(profile)) {
        BleServiceAirbridgeSerial* serial_svc = ble_svc_airbridge_serial_get_active();
        subscribed = serial_svc && ble_svc_airbridge_serial_client_subscribed(serial_svc);
    }
    if(profile) {
        bt_current_profile_release(bt);
    }
    return subscribed;
}

bool bt_serial_tx(const uint8_t* data, uint16_t len) {
    if(!bt_instance) return false;

    Bt* bt = bt_instance;
    bool ret = false;
    /* Reader ref, never the mutex, across the blocking aci_* TX call; the
     * snapshot type check is a pure pointer comparison. */
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
    if(profile) {
        if(bt_profile_is_airbridge(profile)) {
            BleServiceAirbridgeSerial* serial_svc = ble_svc_airbridge_serial_get_active();
            ret = serial_svc &&
                  ble_svc_airbridge_serial_update_tx(serial_svc, (uint8_t*)data, len);
        } else {
            ret = ble_profile_serial_tx(profile, (uint8_t*)data, len);
        }
        bt_current_profile_release(bt);
    }
    return ret;
}

static void bt_draw_statusbar_callback(Canvas* canvas, void* context) {
    furi_assert(context);

    Bt* bt = context;
    uint8_t draw_offset = 0;
    if(bt->beacon_active) {
        canvas_draw_icon(canvas, 0, 0, &I_BLE_beacon_7x8);
        draw_offset += icon_get_width(&I_BLE_beacon_7x8) + ICON_SPACER;
    }
    if(bt->status == BtStatusAdvertising) {
        canvas_draw_icon(canvas, draw_offset, 0, &I_Bluetooth_Idle_5x8);
    } else if(bt->status == BtStatusConnected) {
        canvas_draw_icon(canvas, draw_offset, 0, &I_Bluetooth_Connected_16x8);
    }
}

static ViewPort* bt_statusbar_view_port_alloc(Bt* bt) {
    ViewPort* statusbar_view_port = view_port_alloc();
    view_port_set_width(statusbar_view_port, 5);
    view_port_draw_callback_set(statusbar_view_port, bt_draw_statusbar_callback, bt);
    view_port_enabled_set(statusbar_view_port, false);
    return statusbar_view_port;
}

static void bt_pin_code_view_port_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    Bt* bt = context;
    char pin_code_info[24];
    canvas_draw_icon(canvas, 0, 0, &I_BLE_Pairing_128x64);
    snprintf(pin_code_info, sizeof(pin_code_info), "Pairing code\n%06lu", bt->pin_code);
    elements_multiline_text_aligned(canvas, 64, 4, AlignCenter, AlignTop, pin_code_info);
    elements_button_left(canvas, "Quit");
}

static void bt_pin_code_view_port_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft || event->key == InputKeyBack) {
            view_port_enabled_set(bt->pin_code_view_port, false);
        }
    }
}

static void bt_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Bt* bt = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        const BtMessage msg = {
            .type = BtMessageTypeReloadKeysSettings,
        };

        if(furi_message_queue_put(bt->message_queue, &msg, BT_PRODUCER_QUEUE_TIMEOUT_MS) !=
           FuriStatusOk) {
            FURI_LOG_W(TAG, "SD-triggered BLE reload queue saturated");
        }
    }
}

static ViewPort* bt_pin_code_view_port_alloc(Bt* bt) {
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, bt_pin_code_view_port_draw_callback, bt);
    view_port_input_callback_set(view_port, bt_pin_code_view_port_input_callback, bt);
    view_port_enabled_set(view_port, false);
    return view_port;
}

static void bt_pin_code_show(Bt* bt, uint32_t pin_code) {
    bt->pin_code = pin_code;
    if(!bt->pin_code_view_port) {
        // Pin code view port
        bt->pin_code_view_port = bt_pin_code_view_port_alloc(bt);
        gui_add_view_port(bt->gui, bt->pin_code_view_port, GuiLayerFullscreen);
    }
    notification_message(bt->notification, &sequence_display_backlight_on);
    gui_view_port_send_to_front(bt->gui, bt->pin_code_view_port);
    view_port_enabled_set(bt->pin_code_view_port, true);
}

static void bt_pin_code_hide(Bt* bt) {
    bt->pin_code = 0;
    if(bt->pin_code_view_port && view_port_is_enabled(bt->pin_code_view_port)) {
        view_port_enabled_set(bt->pin_code_view_port, false);
    }
}

static bool bt_pin_code_verify_event_handler(Bt* bt, uint32_t pin) {
    furi_assert(bt);
    notification_message(bt->notification, &sequence_display_backlight_on);
    FuriString* pin_str;
    if(!bt->dialog_message) {
        bt->dialog_message = dialog_message_alloc();
    }
    dialog_message_set_icon(bt->dialog_message, &I_BLE_Pairing_128x64, 0, 0);
    pin_str = furi_string_alloc_printf("Verify code\n%06lu", pin);
    dialog_message_set_text(
        bt->dialog_message, furi_string_get_cstr(pin_str), 64, 4, AlignCenter, AlignTop);
    dialog_message_set_buttons(bt->dialog_message, "Cancel", "OK", NULL);
    DialogMessageButton button = dialog_message_show(bt->dialogs, bt->dialog_message);
    furi_string_free(pin_str);
    return button == DialogMessageButtonCenter;
}

static void bt_battery_level_changed_callback(const void* _event, void* context) {
    furi_assert(_event);
    furi_assert(context);

    Bt* bt = context;
    BtMessage message = {};
    const PowerEvent* event = _event;
    bool is_charging = false;
    switch(event->type) {
    case PowerEventTypeBatteryLevelChanged:
        message.type = BtMessageTypeUpdateBatteryLevel;
        message.data.battery_level = event->data.battery_level;
        furi_check(
            furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
        break;
    case PowerEventTypeStartCharging:
        is_charging = true;
        /* fallthrough */
    case PowerEventTypeFullyCharged:
    case PowerEventTypeStopCharging:
        message.type = BtMessageTypeUpdatePowerState;
        message.data.power_state_charging = is_charging;
        furi_check(
            furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
        break;
    }
}

Bt* bt_alloc(void) {
    Bt* bt = malloc(sizeof(Bt));
    bt->current_profile_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    bt->status_dispatch_mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
    bt->status_callback_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    bt->status_changed_cb = NULL;
    bt->status_changed_ctx = NULL;
    // Init default maximum packet size
    bt->max_packet_size = BLE_PROFILE_SERIAL_PACKET_SIZE_MAX;
    bt->current_profile = NULL;
    bt->current_profile_readers = 0;
    bt->current_profile_is_serial = false;
    bt->current_profile_is_airbridge = false;
    bt->reload_profile_is_airbridge = false;
    bt->profile_retry_pending = false;
    bt->profile_retry_template = NULL;
    bt->pairing_in_progress = false;
    bt->status = BtStatusUnavailable;
    bt->gap_mailbox.status_dirty = false;
    bt->gap_mailbox.status = BtStatusUnavailable;
    bt->gap_mailbox.connect_count = 0;
    bt->gap_mailbox.disconnect_count = 0;
    bt->gap_mailbox.battery_dirty = false;
    bt->gap_mailbox.battery_level = 0;
    bt->gap_mailbox.pin_code_dirty = false;
    bt->gap_mailbox.pin_code = 0;
    bt->flushed_connect_count = 0;
    bt->flushed_disconnect_count = 0;
    bt->flushed_status = BtStatusUnavailable;
    // Keys storage
    bt->keys_storage = bt_keys_storage_alloc(BT_KEYS_STORAGE_PATH);
    // Alloc queue
    bt->message_queue = furi_message_queue_alloc(8, sizeof(BtMessage));

    // Setup statusbar view port
    bt->statusbar_view_port = bt_statusbar_view_port_alloc(bt);
    // Notification
    bt->notification = furi_record_open(RECORD_NOTIFICATION);
    // Gui
    bt->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(bt->gui, bt->statusbar_view_port, GuiLayerStatusBarLeft);

    // Dialogs
    bt->dialogs = furi_record_open(RECORD_DIALOGS);

    // Power
    bt->power = furi_record_open(RECORD_POWER);
    FuriPubSub* power_pubsub = power_get_pubsub(bt->power);
    furi_pubsub_subscribe(power_pubsub, bt_battery_level_changed_callback, bt);

    // RPC
    bt->rpc = furi_record_open(RECORD_RPC);
    bt->rpc_event = furi_event_flag_alloc();

    // API evnent
    bt->api_event = furi_event_flag_alloc();

    return bt;
}

// Called from GAP thread from Serial service
static uint16_t bt_serial_event_callback(SerialServiceEvent event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    uint16_t ret = 0;

    /* The DataReceived path runs with the serial service's buff_size_mtx held,
     * so this callback must NEVER acquire current_profile_mutex (the only
     * allowed lock order is current_profile_mutex -> buff_size_mtx). It reads
     * the cached type flags instead; a momentarily stale flag during a profile
     * change is benign because the BLE link is being torn down in that window. */
    if(event.event == SerialServiceEventTypeDataReceived) {
        if(bt_raw_serial_invoke(event.data.buffer, event.data.size, &ret)) {
            return ret;
        }
        if(bt->current_profile_is_airbridge) {
            return ret;
        }
        size_t bytes_processed =
            rpc_session_feed(bt->rpc_session, event.data.buffer, event.data.size, 1000);
        if(bytes_processed != event.data.size) {
            FURI_LOG_E(
                TAG, "Only %zu of %u bytes processed by RPC", bytes_processed, event.data.size);
        }
        ret = rpc_session_get_available_size(bt->rpc_session);
    } else if(event.event == SerialServiceEventTypeDataSent) {
        bool raw_callback_invoked = bt->current_profile_is_airbridge &&
                                    bt_raw_serial_invoke(NULL, 0, &ret);
        if(!raw_callback_invoked) {
            furi_event_flag_set(bt->rpc_event, BT_RPC_EVENT_BUFF_SENT);
        }
    } else if(event.event == SerialServiceEventTypesBleResetRequest) {
        if(bt->current_profile_is_airbridge) {
            FURI_LOG_W(TAG, "Ignoring reset request for AirBridge profile");
        } else {
            FURI_LOG_I(TAG, "BLE restart request received");
            BtMessage message = {
                .type = BtMessageTypeSetProfile,
                .data.profile.params = NULL,
                .data.profile.template = ble_profile_serial,
            };
            if(furi_message_queue_put(bt->message_queue, &message, BT_PRODUCER_QUEUE_TIMEOUT_MS) !=
               FuriStatusOk) {
                FURI_LOG_W(TAG, "BLE restart request queue saturated");
            }
        }
    }
    return ret;
}

// Called from RPC thread
static void bt_rpc_send_bytes_callback(void* context, uint8_t* bytes, size_t bytes_len) {
    furi_assert(context);
    Bt* bt = context;

    if(furi_event_flag_get(bt->rpc_event) & BT_RPC_EVENT_DISCONNECTED) {
        // Early stop from sending if we're already disconnected
        return;
    }
    furi_event_flag_clear(bt->rpc_event, BT_RPC_EVENT_ALL & (~BT_RPC_EVENT_DISCONNECTED));
    size_t bytes_sent = 0;
    while(bytes_sent < bytes_len) {
        size_t bytes_remain = bytes_len - bytes_sent;
        size_t bytes_to_send = bytes_remain > bt->max_packet_size ? bt->max_packet_size :
                                                                    bytes_remain;
        /* The serial profile may be replaced mid-send; re-validate each chunk.
         * Reader ref only: the mutex is never held across ble_profile_serial_tx
         * (blocking hci_send_req) or the furi_event_flag_wait below. */
        FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
        bool profile_is_serial = furi_hal_bt_check_profile_type(profile, ble_profile_serial);
        if(profile_is_serial) {
            ble_profile_serial_tx(profile, &bytes[bytes_sent], bytes_to_send);
        }
        if(profile) {
            bt_current_profile_release(bt);
        }
        if(!profile_is_serial) {
            FURI_LOG_W(TAG, "Aborting RPC send: serial profile is gone");
            return;
        }
        bytes_sent += bytes_to_send;
        // We want BT_RPC_EVENT_DISCONNECTED to stick, so don't clear
        uint32_t event_flag = furi_event_flag_wait(
            bt->rpc_event, BT_RPC_EVENT_ALL, FuriFlagWaitAny | FuriFlagNoClear, FuriWaitForever);
        if(event_flag & BT_RPC_EVENT_DISCONNECTED) {
            break;
        } else {
            // If we didn't get BT_RPC_EVENT_DISCONNECTED, then clear everything else
            furi_event_flag_clear(bt->rpc_event, BT_RPC_EVENT_ALL & (~BT_RPC_EVENT_DISCONNECTED));
        }
    }
}

static void bt_serial_buffer_is_empty_callback(void* context) {
    furi_assert(context);
    Bt* bt = context;

    /* An in-flight callback can outlive rpc_session_close: the serial profile may
     * already be gone, so guard instead of furi_check. Reader ref only - the
     * mutex is never held across the notify call. */
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
    bool profile_is_serial = furi_hal_bt_check_profile_type(profile, ble_profile_serial);
    if(profile_is_serial) {
        ble_profile_serial_notify_buffer_is_empty(profile);
    }
    if(profile) {
        bt_current_profile_release(bt);
    }

    if(!profile_is_serial) {
        FURI_LOG_W(TAG, "Serial profile is gone, skipping buffer-empty notify");
        // Unblock any RPC sender waiting for BT_RPC_EVENT_BUFF_SENT
        furi_event_flag_set(bt->rpc_event, BT_RPC_EVENT_BUFF_SENT);
    }
}

static void bt_gap_mailbox_wake(Bt* bt) {
    BtMessage message = {.type = BtMessageTypeUpdateStatus};
    (void)furi_message_queue_put(bt->message_queue, &message, 0);
}

static void bt_gap_mailbox_publish_status(Bt* bt, BtStatus status) {
    FURI_CRITICAL_ENTER();
    bt->gap_mailbox.status = status;
    bt->gap_mailbox.status_dirty = true;
    if(status == BtStatusConnected) {
        bt->gap_mailbox.connect_count++;
    } else if(bt->status == BtStatusConnected) {
        bt->gap_mailbox.disconnect_count++;
    }
    bt->status = status;
    FURI_CRITICAL_EXIT();
}

static void bt_gap_mailbox_publish_battery(Bt* bt, uint8_t battery_level) {
    FURI_CRITICAL_ENTER();
    bt->gap_mailbox.battery_level = battery_level;
    bt->gap_mailbox.battery_dirty = true;
    FURI_CRITICAL_EXIT();
}

static void bt_gap_mailbox_mark_status_dirty(Bt* bt) {
    FURI_CRITICAL_ENTER();
    bt->gap_mailbox.status = bt->status;
    bt->gap_mailbox.status_dirty = true;
    FURI_CRITICAL_EXIT();
}

static void bt_gap_mailbox_publish_pin_code(Bt* bt, uint32_t pin_code) {
    FURI_CRITICAL_ENTER();
    bt->gap_mailbox.pin_code = pin_code;
    bt->gap_mailbox.pin_code_dirty = true;
    FURI_CRITICAL_EXIT();
}

static void bt_gap_profile_connected(Bt* bt) {
    if(bt->current_profile_is_airbridge) {
        FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
        if(bt_profile_is_airbridge(profile)) {
            BleServiceAirbridgeSerial* serial_svc = ble_svc_airbridge_serial_get_active();
            if(serial_svc) {
                ble_svc_airbridge_serial_set_callbacks(
                    serial_svc, RPC_BUFFER_SIZE, bt_serial_event_callback, bt);
            } else {
                FURI_LOG_E(TAG, "AirBridge serial service unavailable on connect");
            }
        }
        if(profile) bt_current_profile_release(bt);
    }

    if(bt->current_profile_is_serial) {
        bt->rpc_session = rpc_session_open(bt->rpc, RpcOwnerBle);
        if(bt->rpc_session) {
            FURI_LOG_I(TAG, "Open RPC connection");
            rpc_session_set_send_bytes_callback(bt->rpc_session, bt_rpc_send_bytes_callback);
            rpc_session_set_buffer_is_empty_callback(
                bt->rpc_session, bt_serial_buffer_is_empty_callback);
            rpc_session_set_context(bt->rpc_session, bt);

            FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
            const bool profile_is_serial =
                furi_hal_bt_check_profile_type(profile, ble_profile_serial);
            if(profile_is_serial) {
                ble_profile_serial_set_event_callback(
                    profile, RPC_BUFFER_SIZE, bt_serial_event_callback, bt);
                ble_profile_serial_set_rpc_active(profile, FuriHalBtSerialRpcStatusActive);
            }
            if(profile) bt_current_profile_release(bt);
            if(!profile_is_serial) {
                rpc_session_close(bt->rpc_session);
                bt->rpc_session = NULL;
            }
        } else {
            FURI_LOG_W(TAG, "RPC is busy, failed to open new session");
        }
    }
}

static void bt_gap_profile_disconnected(Bt* bt) {
    /* An AirBridge disconnect must not hold a profile reader while clearing
     * service callbacks: profile replacement waits for readers, while GAP
     * teardown can already own the service state. The profile destructor owns
     * callback cleanup; a plain disconnect keeps them for the next link. */
    if(!bt->current_profile_is_airbridge) {
        FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
        if(furi_hal_bt_check_profile_type(profile, ble_profile_serial)) {
            ble_profile_serial_set_rpc_active(profile, FuriHalBtSerialRpcStatusNotActive);
            ble_profile_serial_set_event_callback(profile, 0, NULL, NULL);
        }
        if(profile) bt_current_profile_release(bt);
    }

    if(bt->rpc_session) {
        FURI_LOG_I(TAG, "Close RPC connection");
        furi_event_flag_set(bt->rpc_event, BT_RPC_EVENT_DISCONNECTED);
        rpc_session_close(bt->rpc_session);
        bt->rpc_session = NULL;
    }
}

// Called from GAP thread
static bool bt_on_gap_event_callback(GapEvent event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    bool ret = false;
    bool wake = false;

    if(event.type == GapEventTypeConnected) {
        if(bt->status == BtStatusConnected) {
            /* Duplicate Connected (gap.c fires at connection-complete, then
             * again at pairing-complete on fresh pairings) must not double-open
             * the RPC session, re-set serial callbacks, or re-send the battery
             * message. This second fire marks ceremony end. */
            bt->pairing_in_progress = false;
            ret = true;
        } else {
            // Update status bar
            bt_gap_mailbox_publish_status(bt, BtStatusConnected);
            wake = true;
            // Clear BT_RPC_EVENT_DISCONNECTED because it might be set from previous session
            furi_event_flag_clear(bt->rpc_event, BT_RPC_EVENT_DISCONNECTED);

            bt_gap_profile_connected(bt);
            // Update battery level
            PowerInfo info;
            power_get_info(bt->power, &info);
            bt_gap_mailbox_publish_battery(bt, info.charge);
            ret = true;
        }
    } else if(event.type == GapEventTypeDisconnected) {
        /* Peer-initiated disconnects must always update status: previously the
         * FAP only learned about a dropped link via subsequent Start/StopAdvertising
         * events, which never arrive when advertising auto-resume fails or is
         * disabled, leaving consumers' connected flag stuck true forever. */
        bt_gap_mailbox_publish_status(bt, BtStatusOff);
        wake = true;
        bt->pairing_in_progress = false;
        bt_gap_profile_disconnected(bt);
        ret = true;
    } else if(event.type == GapEventTypeStartAdvertising) {
        bt_gap_mailbox_publish_status(bt, BtStatusAdvertising);
        wake = true;
        ret = true;
    } else if(event.type == GapEventTypeStopAdvertising) {
        bt_gap_mailbox_publish_status(bt, BtStatusOff);
        wake = true;
        ret = true;
    } else if(event.type == GapEventTypePinCodeShow) {
        bt->pairing_in_progress = true;
        bt_gap_mailbox_publish_pin_code(bt, event.data.pin_code);
        wake = true;
        ret = true;
    } else if(event.type == GapEventTypePinCodeVerify) {
        bt->pairing_in_progress = true;
        ret = bt_pin_code_verify_event_handler(bt, event.data.pin_code);
    } else if(event.type == GapEventTypeUpdateMTU) {
        bt->max_packet_size = event.data.max_packet_size;
        ret = true;
    } else if(event.type == GapEventTypeBeaconStart) {
        bt->beacon_active = true;
        bt_gap_mailbox_mark_status_dirty(bt);
        wake = true;
        ret = true;
    } else if(event.type == GapEventTypeBeaconStop) {
        bt->beacon_active = false;
        bt_gap_mailbox_mark_status_dirty(bt);
        wake = true;
        ret = true;
    }

    if(wake) bt_gap_mailbox_wake(bt);
    return ret;
}

static bool bt_gap_mailbox_deliver_status(Bt* bt, BtStatus status) {
    const BtStatusRegistration registration = bt_status_registration_make(bt);
    return bt_status_callback_deliver_bounded(
        &registration, status, BT_STATUS_CALLBACK_TIMEOUT_MS);
}

static void bt_gap_mailbox_flush(Bt* bt) {
    bool status_dirty;
    BtStatus status;
    uint32_t connect_count;
    uint32_t disconnect_count;
    bool battery_dirty;
    uint8_t battery_level;
    bool pin_code_dirty;
    uint32_t pin_code;

    FURI_CRITICAL_ENTER();
    status_dirty = bt->gap_mailbox.status_dirty;
    status = bt->gap_mailbox.status;
    connect_count = bt->gap_mailbox.connect_count;
    disconnect_count = bt->gap_mailbox.disconnect_count;
    battery_dirty = bt->gap_mailbox.battery_dirty;
    battery_level = bt->gap_mailbox.battery_level;
    pin_code_dirty = bt->gap_mailbox.pin_code_dirty;
    pin_code = bt->gap_mailbox.pin_code;
    bt->gap_mailbox.status_dirty = false;
    bt->gap_mailbox.battery_dirty = false;
    bt->gap_mailbox.pin_code_dirty = false;
    FURI_CRITICAL_EXIT();

    if(status_dirty) {
        uint32_t pending_connects = connect_count - bt->flushed_connect_count;
        uint32_t pending_disconnects = disconnect_count - bt->flushed_disconnect_count;
        bool delivered_connected = bt->flushed_status == BtStatusConnected;

        while(pending_connects || pending_disconnects) {
            if(!delivered_connected && pending_connects) {
                if(!bt_gap_mailbox_deliver_status(bt, BtStatusConnected)) goto delivery_deferred;
                pending_connects--;
                delivered_connected = true;
            } else if(delivered_connected && pending_disconnects) {
                if(!bt_gap_mailbox_deliver_status(bt, BtStatusOff)) goto delivery_deferred;
                pending_disconnects--;
                delivered_connected = false;
            } else if(pending_disconnects) {
                pending_disconnects--;
            } else {
                pending_connects--;
            }
        }

        bt->flushed_connect_count = connect_count;
        bt->flushed_disconnect_count = disconnect_count;
        if(bt->flushed_status != status && !bt_gap_mailbox_deliver_status(bt, status)) {
            goto delivery_deferred;
        }
        bt_statusbar_update(bt);
        bt_pin_code_hide(bt);
    }

    if(battery_dirty) furi_hal_bt_update_battery_level(battery_level);
    if(pin_code_dirty) bt_pin_code_show(bt, pin_code);
    return;

delivery_deferred: {
    FURI_CRITICAL_ENTER();
    bt->gap_mailbox.status_dirty = true;
    FURI_CRITICAL_EXIT();
}
}

static void bt_on_key_storage_change_callback(uint8_t* addr, uint16_t size, void* context) {
    furi_assert(context);
    Bt* bt = context;
    BtMessage message = {
        .type = BtMessageTypeKeysStorageUpdated,
        .data.key_storage_data.start_address = addr,
        .data.key_storage_data.size = size};
    if(furi_message_queue_put(bt->message_queue, &message, BT_PRODUCER_QUEUE_TIMEOUT_MS) !=
       FuriStatusOk) {
        FURI_LOG_W(TAG, "BLE key update queue saturated");
    }
}

static void bt_statusbar_update(Bt* bt) {
    uint8_t active_icon_width = 0;
    if(bt->beacon_active) {
        active_icon_width = icon_get_width(&I_BLE_beacon_7x8) + ICON_SPACER;
    }
    if(bt->status == BtStatusAdvertising) {
        active_icon_width += icon_get_width(&I_Bluetooth_Idle_5x8);
    } else if(bt->status == BtStatusConnected) {
        active_icon_width += icon_get_width(&I_Bluetooth_Connected_16x8);
    }

    if(active_icon_width > 0) {
        view_port_set_width(bt->statusbar_view_port, active_icon_width);
        view_port_enabled_set(bt->statusbar_view_port, true);
    } else {
        view_port_enabled_set(bt->statusbar_view_port, false);
    }
}

static void bt_show_warning(Bt* bt, const char* text) {
    if(!bt->dialog_message) {
        bt->dialog_message = dialog_message_alloc();
    }
    dialog_message_set_text(bt->dialog_message, text, 64, 28, AlignCenter, AlignCenter);
    dialog_message_set_buttons(bt->dialog_message, "Quit", NULL, NULL);
    dialog_message_show(bt->dialogs, bt->dialog_message);
}

static void bt_close_rpc_connection(Bt* bt) {
    /* Runs only on the BtSrv thread (the sole current_profile writer), called
     * with no mutex held. The reader ref keeps the profile alive across
     * rpc_session_close (an in-flight RPC callback may still be using it);
     * it is released before returning. */
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(bt);
    bool profile_is_serial = furi_hal_bt_check_profile_type(profile, ble_profile_serial);

    if(profile_is_serial && bt->rpc_session) {
        FURI_LOG_I(TAG, "Close RPC connection");
        furi_event_flag_set(bt->rpc_event, BT_RPC_EVENT_DISCONNECTED);
        rpc_session_close(bt->rpc_session);
        ble_profile_serial_set_event_callback(profile, 0, NULL, NULL);
        bt->rpc_session = NULL;
    }
    if(profile) {
        bt_current_profile_release(bt);
    }
}

static void bt_schedule_profile_retry(Bt* bt, const BtMessage* message) {
    const FuriHalBleProfileTemplate* profile_template = message->data.profile.template;
    if(profile_template != ble_profile_serial && profile_template != ble_profile_airbridge) return;

    bt->profile_retry_template = profile_template;
    if(profile_template == ble_profile_airbridge) {
        furi_check(message->data.profile.params);
        bt->profile_retry_airbridge_params =
            *(const AirbridgeBleIdentityParams*)message->data.profile.params;
    }
    bt->profile_retry_pending = true;
}

static void bt_change_profile(Bt* bt, BtMessage* message) {
    if(furi_hal_bt_is_gatt_gap_supported()) {
        bt_settings_load(&bt->bt_settings);

        // Close RPC first, with no profile mutex held (see bt_close_rpc_connection)
        bt_close_rpc_connection(bt);

        bt_keys_storage_load(bt->keys_storage);

        /* Publish NULL before reinit: post-publish readers see the
         * NULL/type-guarded bail path and start no new reader refs, so
         * wait_quiescent returns as soon as in-flight readers drain; only then
         * may furi_hal_bt_change_app free the old profile. Never hold the
         * mutex across the wait or furi_hal_bt_change_app: its GAP stop path
         * waits for the GAP thread, which invokes bt_on_gap_event_callback. */
        FuriHalBleProfileBase* previous_profile = NULL;
        if(!bt_unpublish_current_profile_bounded(bt, &previous_profile)) {
            bt_schedule_profile_retry(bt, message);
            if(message->profile_instance) *message->profile_instance = NULL;
            if(message->result) *message->result = false;
            return;
        }

        if(!bt_current_profile_wait_quiescent(bt)) {
            if(!bt_publish_current_profile_bounded(bt, previous_profile)) {
                FURI_LOG_E(TAG, "Profile republish timed out");
            }
            bt_schedule_profile_retry(bt, message);
            if(message->profile_instance) {
                *message->profile_instance = NULL;
            }
            if(message->result) {
                *message->result = false;
            }
            return;
        }

        const bool requested_airbridge = message->data.profile.template == ble_profile_airbridge;
        if(requested_airbridge) furi_check(message->data.profile.params);

        FuriHalBleProfileBase* new_profile = furi_hal_bt_change_app(
            message->data.profile.template,
            message->data.profile.params,
            bt_keys_storage_get_root_keys(bt->keys_storage),
            bt_on_gap_event_callback,
            bt);

        if(!bt_publish_current_profile_bounded(bt, new_profile)) {
            FURI_LOG_E(TAG, "Profile publication timed out");
            bt_schedule_profile_retry(bt, message);
            new_profile = NULL;
        }

        if(new_profile) {
            bt->reload_profile_is_airbridge = requested_airbridge;
            if(requested_airbridge) {
                bt->reload_airbridge_params =
                    *(const AirbridgeBleIdentityParams*)message->data.profile.params;
            }
            FURI_LOG_I(TAG, "Bt App started");
            if(bt->bt_settings.enabled) {
                furi_hal_bt_start_advertising();
            }
            furi_hal_bt_set_key_storage_change_callback(bt_on_key_storage_change_callback, bt);
        } else {
            FURI_LOG_E(TAG, "Failed to start Bt App");
            bt_schedule_profile_retry(bt, message);
        }
        if(message->profile_instance) {
            *message->profile_instance = new_profile;
        }
        if(message->result) {
            *message->result = new_profile != NULL;
        }

    } else {
        bt_show_warning(bt, "Radio stack doesn't support this app");
        if(message->result) {
            *message->result = false;
        }
        if(message->profile_instance) {
            *message->profile_instance = NULL;
        }
    }
}

static void bt_close_connection(Bt* bt) {
    bt_close_rpc_connection(bt);
    furi_hal_bt_stop_advertising();
}

static void bt_apply_settings(Bt* bt) {
    if(bt->bt_settings.enabled) {
        furi_hal_bt_start_advertising();
    } else {
        furi_hal_bt_stop_advertising();
    }
}

static FuriHalBleProfileBase* bt_load_keys(Bt* bt) {
    FuriHalBleProfileBase* previous_profile = NULL;
    if(!furi_hal_bt_is_gatt_gap_supported()) {
        bt_show_warning(bt, "Unsupported radio stack");
        bt->status = BtStatusUnavailable;
        return NULL;

    } else if(bt_keys_storage_is_changed(bt->keys_storage)) {
        FURI_LOG_I(TAG, "Loading new keys");

        bt_close_rpc_connection(bt);
        bt_keys_storage_load(bt->keys_storage);

        /* Reachable at runtime via BtMessageTypeReloadKeysSettings (SD card
         * mount event), so the NULL publish is mutex-protected like any write. */
        if(!bt_unpublish_current_profile_bounded(bt, &previous_profile)) {
            FURI_LOG_W(TAG, "Profile unpublish timed out");
        }
    } else {
        FURI_LOG_I(TAG, "Keys unchanged");
    }
    return previous_profile;
}

static void bt_start_application(Bt* bt, FuriHalBleProfileBase* previous_profile) {
    const BtMessage retry_message = {
        .type = BtMessageTypeSetProfile,
        .data.profile.template = bt->reload_profile_is_airbridge ? ble_profile_airbridge :
                                                                   ble_profile_serial,
        .data.profile.params = bt->reload_profile_is_airbridge ? &bt->reload_airbridge_params :
                                                                 NULL,
    };
    bool profile_start_needed = false;
    if(!bt_current_profile_is_null_bounded(bt, &profile_start_needed)) {
        FURI_LOG_W(TAG, "Profile state snapshot timed out");
        bt_schedule_profile_retry(bt, &retry_message);
        return;
    }

    if(profile_start_needed) {
        const FuriHalBleProfileTemplate* reload_template = retry_message.data.profile.template;
        FuriHalBleProfileParams reload_params = retry_message.data.profile.params;
        /* Only the BtSrv thread writes current_profile, so the check above
         * cannot go stale. The pointer is already NULL (new readers bail and
         * start no new refs), so drain any pre-NULL reader refs before
         * furi_hal_bt_change_app frees the old profile. The new profile is
         * published under the mutex. */
        if(!bt_current_profile_wait_quiescent(bt)) {
            if(!bt_publish_current_profile_bounded(bt, previous_profile)) {
                FURI_LOG_E(TAG, "Profile republish timed out");
            }
            bt_schedule_profile_retry(bt, &retry_message);
            FURI_LOG_W(TAG, "Profile reload deferred: readers did not quiesce");
            return;
        }
        FuriHalBleProfileBase* profile = furi_hal_bt_change_app(
            reload_template,
            reload_params,
            bt_keys_storage_get_root_keys(bt->keys_storage),
            bt_on_gap_event_callback,
            bt);

        if(!bt_publish_current_profile_bounded(bt, profile)) {
            FURI_LOG_E(TAG, "Profile publication timed out");
            bt_schedule_profile_retry(bt, &retry_message);
            profile = NULL;
        }

        if(!profile) {
            FURI_LOG_E(TAG, "BLE App start failed");
            bt->status = BtStatusUnavailable;
        }
    }
}

static void bt_load_settings(Bt* bt) {
    bt_settings_load(&bt->bt_settings);
    bt_apply_settings(bt);
}

static void bt_handle_get_settings(Bt* bt, BtMessage* message) {
    *message->data.settings = bt->bt_settings;
}

static void bt_handle_set_settings(Bt* bt, BtMessage* message) {
    bt->bt_settings = *message->data.csettings;
    bt_apply_settings(bt);
    bt_settings_save(&bt->bt_settings);
}

static void bt_handle_reload_keys_settings(Bt* bt) {
    FuriHalBleProfileBase* previous_profile = bt_load_keys(bt);
    bt_start_application(bt, previous_profile);
    bt_load_settings(bt);
}

static void bt_init_keys_settings(Bt* bt) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), bt_storage_callback, bt);

    if(storage_sd_status(storage) != FSE_OK) {
        FURI_LOG_D(TAG, "SD Card not ready, skipping settings");

        // Just start the BLE serial application without loading the keys or settings
        bt_start_application(bt, NULL);
        return;
    }

    bt_handle_reload_keys_settings(bt);
}

int32_t bt_srv(void* p) {
    UNUSED(p);
    Bt* bt = bt_alloc();
    bt_instance = bt;

    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        FURI_LOG_W(TAG, "Skipping start in special boot mode");
        ble_glue_wait_for_c2_start(FURI_HAL_BT_C2_START_TIMEOUT);
        furi_record_create(RECORD_BT, bt);

        furi_thread_suspend(furi_thread_get_current_id());
        return 0;
    }

    if(furi_hal_bt_start_radio_stack()) {
        bt_init_keys_settings(bt);
        furi_hal_bt_set_key_storage_change_callback(bt_on_key_storage_change_callback, bt);

    } else {
        FURI_LOG_E(TAG, "Radio stack start failed");
    }

    furi_record_create(RECORD_BT, bt);

    BtMessage message;

    while(1) {
        const uint32_t queue_timeout = bt->profile_retry_pending ? BT_PROFILE_RETRY_DELAY_MS :
                                                                   FuriWaitForever;
        FuriStatus queue_status =
            furi_message_queue_get(bt->message_queue, &message, queue_timeout);
        if(queue_status == FuriStatusErrorTimeout && bt->profile_retry_pending) {
            message = (BtMessage){
                .type = BtMessageTypeSetProfile,
                .data.profile.template = bt->profile_retry_template,
                .data.profile.params = bt->profile_retry_template == ble_profile_airbridge ?
                                           &bt->profile_retry_airbridge_params :
                                           NULL,
            };
            bt->profile_retry_pending = false;
            queue_status = FuriStatusOk;
        }
        furi_check(queue_status == FuriStatusOk);
        FURI_LOG_D(
            TAG,
            "call %d, lock 0x%p, result 0x%p",
            message.type,
            (void*)message.lock,
            (void*)message.result);
        if(message.type == BtMessageTypeUpdateBatteryLevel) {
            // Update battery level
            furi_hal_bt_update_battery_level(message.data.battery_level);
        } else if(message.type == BtMessageTypeUpdatePowerState) {
            furi_hal_bt_update_power_state(message.data.power_state_charging);
        } else if(message.type == BtMessageTypePinCodeShow) {
            // Display PIN code
            bt_pin_code_show(bt, message.data.pin_code);
        } else if(message.type == BtMessageTypeKeysStorageUpdated) {
            bt_keys_storage_update(
                bt->keys_storage,
                message.data.key_storage_data.start_address,
                message.data.key_storage_data.size);
        } else if(message.type == BtMessageTypeSetProfile) {
            bt->profile_retry_pending = false;
            bt_change_profile(bt, &message);
        } else if(message.type == BtMessageTypeDisconnect) {
            bt_close_connection(bt);
        } else if(message.type == BtMessageTypeForgetBondedDevices) {
            bt_keys_storage_delete(bt->keys_storage);
        } else if(message.type == BtMessageTypeGetSettings) {
            bt_handle_get_settings(bt, &message);
        } else if(message.type == BtMessageTypeSetSettings) {
            bt_handle_set_settings(bt, &message);
        } else if(message.type == BtMessageTypeReloadKeysSettings) {
            bt_handle_reload_keys_settings(bt);
        }

        bt_gap_mailbox_flush(bt);
        if(message.lock) api_lock_unlock(message.lock);
    }

    return 0;
}
