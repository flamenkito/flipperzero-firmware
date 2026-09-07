#include <furi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_airbridge.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include <bt/bt_service/bt.h>
#include "airbridge_ble.h"
#include "airbridge_exit_contract.h"
#include "airbridge_config.h"
#include "airbridge_relay.h"
#include "airbridge_stream.h"
#include "airbridge_typing.h"
#include "airbridge_types.h"
#include "airbridge_ui.h"

#define TAG "AirBridge"

struct AirbridgeApp {
    bool running;
    volatile bool exit_requested;
    Storage* storage;
    AirbridgeBle ble;
    AirbridgeConfig config;
    AirbridgeRelay relay;
    AirbridgeStream stream;
    AirbridgeTyping typing;
    AirbridgeUi ui;
    AirbridgeScreen screen;
    AirbridgeError error;
};

static uint32_t last_heartbeat;

static void app_set_error(AirbridgeError* error, const char* title, const char* detail, const char* action) {
    snprintf(error->title, sizeof(error->title), "%s", title);
    snprintf(error->detail, sizeof(error->detail), "%s", detail);
    snprintf(error->action, sizeof(error->action), "%s", action);
}

static void app_describe_error(AirbridgeError* error, const char* message) {
    const char* title = "Deploy error";
    const char* detail = message;
    const char* action = "Retry after BACK";
    if(strstr(message, "bootstrap") != NULL || strstr(message, "BYTE 0x") != NULL) {
        title = strstr(message, "NO ") == message ? "Bootstrap missing" : "Invalid bootstrap";
        action = "Fix asset, then BACK";
    } else if(strstr(message, "app-usb") != NULL || strstr(message, "app-ble") != NULL) {
        title = "App bundle unavailable";
        action = "Copy bundle, then BACK";
    } else if(strcmp(message, "KEYBOARD SEND ERROR") == 0) {
        title = "Keyboard link lost";
        detail = "Could not send a key";
        action = "Check host; tap a key";
    } else if(strcmp(message, "BLE LINK TIMEOUT") == 0) {
        title = "BLE link timed out";
        detail = "Keyboard host disconnected";
        action = "Reconnect, then BACK";
    } else if(strstr(message, "STREAM") != NULL) {
        title = "Transfer failed";
        detail = strcmp(message, "STREAM STALLED") == 0 ? "BLE stream stalled" : "Bundle stream stopped";
    } else if(strcmp(message, "DEPLOY NOT ARMED") == 0) {
        title = "Deploy rejected";
        detail = "Request outside Waiting";
        action = "BACK, then arm deploy";
    } else if(strcmp(message, "STUCK KEY - TAP A KEY") == 0) {
        title = "Key release failed";
        detail = "Key may remain pressed";
        action = "Tap a key, then BACK";
    } else if(strcmp(message, "Set USB to Kbd+Vendor") == 0) {
        title = "USB profile cannot type";
        detail = "Select Kbd+Vendor profile";
    }
    app_set_error(error, title, detail, action);
}

static void app_show_error(AirbridgeApp* app, const char* message) {
    if(app->screen == AirbridgeScreenError) {
        if(strcmp(message, "STUCK KEY - TAP A KEY") == 0) {
            app_describe_error(&app->error, message);
            return;
        }
        FURI_LOG_W(TAG, "Preserving first error; ignored: %s", message);
        return;
    }
    if(app->screen == AirbridgeScreenTyping) {
        if(!airbridge_typing_abort(&app->typing)) {
            app_describe_error(&app->error, "STUCK KEY - TAP A KEY");
            app->screen = AirbridgeScreenError;
            return;
        }
    } else if(app->screen == AirbridgeScreenStreaming) {
        airbridge_stream_close(&app->stream);
    }
    app_describe_error(&app->error, message);
    app->screen = AirbridgeScreenError;
}

static void app_show_fatal(AirbridgeApp* app, const char* message) {
    app_set_error(&app->error, "Fatal startup error", message, "Long BACK: exit");
    app->screen = AirbridgeScreenFatal;
}

static void app_request_exit(AirbridgeApp* app) {
    app->exit_requested = true;
    airbridge_relay_wake(&app->relay);
}

static bool app_signal_callback(uint32_t signal, void* arg, void* context) {
    UNUSED(arg);
    if(signal != FuriSignalExit) return false;
    app_request_exit(context);
    return true;
}

static void app_service_input_callback(AirbridgeApp* app) {
    airbridge_ui_service_input(&app->ui);
}

static bool app_start_stream_callback(AirbridgeApp* app) {
    bool started = airbridge_stream_start(&app->stream);
    if(started) app->ble.deploy_request_accepted = true;
    return started;
}

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);
    AirbridgeApp* app = malloc(sizeof(*app));
    if(app == NULL) return -1;
    memset(app, 0, sizeof(*app));
    app->screen = AirbridgeScreenBridge;
    app->config.usb_profile_index = FuriHalUsbAirbridgeProfileHpKbdVendor;
    app->relay.metrics.chunks_usb_to_ble = 0;
    app->relay.metrics.chunks_ble_to_usb = 0;
    app->relay.metrics.dropped = 0;
    app->relay.metrics.tx_errors = 0;
    app->relay.metrics.usb_connected = false;
    last_heartbeat = furi_get_tick();

    /* No NULL checks on the allocs below: OOM is unrecoverable by firmware
     * design — view_port_alloc/storage_file_alloc deref their malloc before
     * returning and furi_message_queue_alloc furi_checks internally, so a
     * failed alloc crashes inside the allocator and caller-side checks would
     * be dead code. The app-struct malloc above keeps its check (plain malloc
     * returns NULL instead of crashing). */
    airbridge_relay_init(&app->relay);
    airbridge_ui_init(
        &app->ui,
        &app->running,
        &app->exit_requested,
        &app->screen,
        &app->config,
        &app->relay,
        &app->ble,
        &app->typing,
        &app->stream,
        &app->error,
        app_show_error,
        app);
    app->storage = furi_record_open(RECORD_STORAGE);
    airbridge_typing_init(
        &app->typing,
        app->storage,
        &app->ble,
        &app->screen,
        &app->stream.started_tick,
        &app->stream.done_since,
        app_service_input_callback,
        app_show_error,
        app);
    airbridge_stream_init(
        &app->stream,
        app->storage,
        &app->typing.transport,
        &app->screen,
        app_show_error,
        app);
    airbridge_ui_init_view(&app->ui);

    airbridge_ui_open_gui(&app->ui);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    airbridge_ui_add_view(&app->ui);

    airbridge_config_load(&app->config, app->storage);
    app->relay.usb_mode_prev = furi_hal_usb_get_config();

    FuriThread* app_thread = furi_thread_get_current();
    furi_thread_set_signal_callback(app_thread, app_signal_callback, app);

    bool startup_apply_pending = true;
run_app:
    app->running = true;
    while(app->running) {
        airbridge_ui_service_input(&app->ui);
        if(app->exit_requested) {
            app->running = false;
            break;
        }

        airbridge_ble_service_pending(&app->ble);

        BridgeEvent be;
        FuriStatus status = airbridge_relay_poll(&app->relay, &be, 10);
        if(status == FuriStatusOk) {
            if(be.type == EVENT_TYPE_RELAY) {
                if(!be.to_ble) airbridge_ble_note_rx(&app->ble, be.tick);
                airbridge_relay_handle(
                    &app->relay,
                    &be,
                    app->screen,
                    app_start_stream_callback,
                    app_show_error,
                    app);
            } else if(be.type == EVENT_TYPE_USB) {
                airbridge_relay_set_usb_connected(&app->relay, be.to_ble);
            }
        }

        if(app->exit_requested) {
            app->running = false;
            break;
        }

        if(startup_apply_pending) {
            // Apply the USB composite profile FIRST, then install the AirBridge BLE profile.
            // The BLE profile start runs furi_hal_bt_reinit() (core2 reset), which in the old
            // order happened before USB apply and left the vendor OUT endpoint unreachable
            // (USB IN worked, U->B counter stayed 0). Applying USB first lets the host enumerate
            // the composite descriptor before any BLE-side core reset activity can disturb it.
            startup_apply_pending = false;
            const bool usb_ready = airbridge_relay_configure_usb(
                &app->relay,
                app->config.usb_profile_index,
                &app->config.usb_profile_index);
            if(!usb_ready) {
                app_show_fatal(app, "USB CONFIG ERROR");
            } else if(!airbridge_ble_configure(&app->ble, &app->config.ble_identity)) {
                if(!airbridge_relay_restore_usb(&app->relay)) {
                    FURI_LOG_E(TAG, "USB restore failed after BLE startup error");
                }
                app_show_fatal(app, "BLE CONFIG ERROR");
            }
        }

        /* Deferred BLE release-all from app_abort_typing: ONE direct attempt
         * per loop iteration, no retry wrapper — the retry path could block
         * ~1 s per attempt in gatt, which is exactly the abort-latency budget
         * this design removes from the abort path. Serviced through Bt using
         * the latched BLE session state (never mutable typing_transport) and BEFORE the screen
         * dispatch so a pending release lands ahead of any new typing step. */
        const bool typing_release_ready = airbridge_typing_service_release(&app->typing);

        if(app->screen == AirbridgeScreenBridge) {
            airbridge_ble_bridge_adv_watchdog(&app->ble);
            /* Squatter watchdog runs on Bridge: the picker window matters here
             * and a bonded macOS HID daemon otherwise holds the link forever. */
            airbridge_ble_squatter_watchdog(&app->ble);
        } else if(app->screen == AirbridgeScreenTyping && typing_release_ready) {
            /* NO squatter kick on Typing: the HID host link is legitimate AND
             * never subscribes the AirBridge TX CCCD, and typing runs 60-90 s —
             * a 15 s kick would corrupt the typed stream mid-bootstrap. */
            airbridge_typing_step(&app->typing);
        } else if(app->screen == AirbridgeScreenStreaming) {
            /* NO squatter kick on Streaming: an active bundle/file transfer is
             * definitionally a subscribed, legitimate client. */
            if(app->typing.transport == AirbridgeTypingTransportBle) {
                airbridge_stream_step_ble(&app->stream);
            } else {
                airbridge_stream_step_usb(&app->stream);
            }
        } else if(app->screen == AirbridgeScreenWaiting) {
            if(app->typing.transport == AirbridgeTypingTransportBle) {
                uint32_t now = furi_get_tick();
                if(now - app->ble.ble_waiting_last_pump_tick >= BLE_WAITING_PUMP_MS) {
                    app->ble.ble_waiting_last_pump_tick = now;
                    // This starts advertising only from GapStateIdle. During a link or
                    // numeric-comparison pairing, GAP is already active, so never
                    // disconnect it merely to recover a genuinely idle advertiser.
                    FURI_LOG_D(TAG, "BLE Waiting pump: restart adv if idle");
                    airbridge_ble_ensure_serial_adv(&app->ble);
                }
                /* Squatter kick (Waiting): a bonded macOS HID daemon
                 * auto-reconnects the keyboard and sits on the link without ever
                 * subscribing the AirBridge TX CCCD, keeping the Web Bluetooth
                 * picker empty. Chrome's bootstrap subscribes TX immediately,
                 * so any central still unsubscribed past the deadline is safe to
                 * kick. One kick per connection by design: the next rising edge
                 * re-stamps ble_connected_since. 15 s budget (not shorter): on a
                 * FRESH origin the first bootstrap Connect runs a pairing
                 * ceremony (numeric code shown on the Flipper + human reaction
                 * time) BEFORE the TX subscription lands; hardware showed a 3 s
                 * window killing that first connect mid-pairing. */
                airbridge_ble_squatter_watchdog(&app->ble);
                /* Waiting zombie-kick: a SUBSCRIBED zombie from a failed
                 * bootstrap attempt (Chrome's gatt.disconnect() does not
                 * reliably drop the OS link) survives the squatter watchdog
                 * and blocks advertising forever. On Waiting the only legit
                 * link progress is the 0x42 deploy write, and
                 * deploy_request_accepted is reset per connection, so arbitrary
                 * BLE writes cannot mask a link with no accepted request. */
                if(app->ble.ble_profile_installed && app->ble.ble_connected &&
                   !app->ble.deploy_request_accepted &&
                   furi_get_tick() - app->ble.ble_connected_since >=
                       BLE_WAITING_ZOMBIE_KICK_MS) {
                    FURI_LOG_W(TAG, "BLE Waiting: kicking subscribed zombie link");
                    airbridge_ble_force_reconnect(&app->ble);
                }
            }
        } else if(app->screen == AirbridgeScreenDone) {
            /* Zombie-kick: the ONLY client allowed to span the stream is the
             * deploy bootstrap, and it must be disconnected by Done
             * (browser-side gatt.disconnect() is not reliable — a subscribed
             * zombie holds the link invisibly and starves the part-3 picker).
             * Any link still up whose connect predates the stream start is that
             * zombie by definition; a legit next client connects only after
             * Done and gets a fresh timestamp. */
            if(app->ble.ble_profile_installed && app->ble.ble_connected &&
               app->stream.started_tick != 0 &&
               app->ble.ble_connected_since < app->stream.started_tick &&
               furi_get_tick() - app->stream.done_since >= BLE_DONE_ZOMBIE_GRACE_MS) {
                FURI_LOG_W(TAG, "BLE Done: kicking bootstrap zombie link");
                airbridge_ble_force_reconnect(&app->ble);
            }
            /* Done: the picker window matters again (the user may start a new
             * transfer from either page), so a squatter holding the link here
             * wedges Web Bluetooth exactly like on Bridge/Waiting. */
            airbridge_ble_squatter_watchdog(&app->ble);
        } else if(app->screen == AirbridgeScreenError) {
            /* Error: same picker-window rationale as Done. */
            airbridge_ble_squatter_watchdog(&app->ble);
        }
        /* NO squatter kick on DeployPrompt: the pairing ceremony at the prompt
         * (numeric comparison + human reaction time) can exceed 15 s before the
         * TX CCCD subscription lands, and kicking there aborts the deploy. */
        airbridge_ui_update(&app->ui);

        if(furi_get_tick() - last_heartbeat >= 500) {
            notification_message(notifications, &sequence_blink_green_100);
            last_heartbeat = furi_get_tick();
        }
    }

    Bt* bt = NULL;
    AirbridgeExitContract exit_contract = airbridge_exit_contract_initial();
    if(!airbridge_exit_contract_record_detach(
           &exit_contract, airbridge_ble_prepare_restore(&app->ble, &bt))) {
        FURI_LOG_E(TAG, "BLE callback detach timed out; retaining app ownership");
        app->exit_requested = false;
        goto run_app;
    }

    furi_check(airbridge_exit_contract_may_destroy(&exit_contract));
    furi_thread_set_signal_callback(app_thread, NULL, NULL);
    airbridge_stream_close(&app->stream);
    (void)airbridge_typing_abort(&app->typing);
    if(!airbridge_relay_restore_usb(&app->relay)) {
        FURI_LOG_E(TAG, "Failed to queue USB restore at teardown");
    }
    airbridge_ui_remove_view(&app->ui);
    airbridge_ui_close_gui();
    furi_record_close(RECORD_NOTIFICATION);
    airbridge_typing_release_file(&app->typing);
    airbridge_stream_deinit(&app->stream);
    furi_record_close(RECORD_STORAGE);
    airbridge_ui_free_view(&app->ui);
    airbridge_ui_deinit(&app->ui);
    airbridge_relay_deinit(&app->relay);
    airbridge_typing_deinit(&app->typing);
    free(app);
    airbridge_ble_queue_restore(bt);

    return 0;
}
