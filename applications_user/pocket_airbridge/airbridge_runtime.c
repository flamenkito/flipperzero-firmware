#include "airbridge_runtime.h"

#include <stdio.h>

#include "airbridge_usb.h"
#include "airbridge_utilities.h"
#include <notification/notification_messages.h>

#define TAG "AirBridge"

static bool airbridge_runtime_typing_start(void* context, AirbridgeTypingTransport transport) {
    AirbridgeApp* app = context;
    const bool started = airbridge_typing_start(&app->typing, transport);
    if(started) {
        /* New deploy session: invalidate any stale stream stamp so a later
         * Done screen never zombie-kicks against a previous session. */
        app->stream.started_tick = 0;
        app->stream.done_since = 0;
    }
    return started;
}

static bool airbridge_runtime_typing_abort(void* context) {
    AirbridgeApp* app = context;
    return airbridge_typing_abort(&app->typing);
}

static bool airbridge_runtime_stream_start(void* context, AirbridgeStreamTransport transport) {
    AirbridgeApp* app = context;
    return airbridge_stream_start(&app->stream, transport);
}

static void airbridge_runtime_stream_close(void* context) {
    AirbridgeApp* app = context;
    airbridge_stream_close(&app->stream);
}

static void airbridge_runtime_ble_reset(void* context) {
    AirbridgeApp* app = context;
    airbridge_operation_start(&app->operation, AirbridgeOperationBleReconnect, furi_get_tick());
    airbridge_ble_force_reconnect(&app->ble);
    airbridge_operation_end(&app->operation);
}

static bool airbridge_runtime_ble_profile_installed(void* context) {
    const AirbridgeApp* app = context;
    return app->ble.ble_profile_installed;
}

static bool airbridge_runtime_deploy_supported(void* context, AirbridgeTypingTransport transport) {
    const AirbridgeApp* app = context;
    if(transport == AirbridgeTypingTransportBle) {
        return app->ble.ble_profile_installed;
    }
    return airbridge_usb_profile_has_keyboard(app->config.usb_profile_index);
}

static void airbridge_runtime_exit(void* context) {
    AirbridgeApp* app = context;
    __atomic_store_n(&app->exit_requested, true, __ATOMIC_RELEASE);
    airbridge_relay_wake(&app->relay);
}

static void airbridge_runtime_input_dropped(void* context) {
    AirbridgeApp* app = context;
    airbridge_relay_count_drop(&app->relay);
}

const AirbridgeScreenActions* airbridge_runtime_screen_actions(void) {
    static const AirbridgeScreenActions actions = {
        .typing_start = airbridge_runtime_typing_start,
        .typing_abort = airbridge_runtime_typing_abort,
        .stream_start = airbridge_runtime_stream_start,
        .stream_close = airbridge_runtime_stream_close,
        .ble_reset = airbridge_runtime_ble_reset,
        .ble_profile_installed = airbridge_runtime_ble_profile_installed,
        .deploy_supported = airbridge_runtime_deploy_supported,
        .exit = airbridge_runtime_exit,
        .input_dropped = airbridge_runtime_input_dropped,
        .menu_enter = airbridge_utilities_enter,
        .menu_move = airbridge_utilities_move,
        .menu_confirm = airbridge_utilities_confirm,
    };
    return &actions;
}

static bool
    airbridge_runtime_exit_requested(const AirbridgeApp* app, AirbridgeExitLatch* exit_latch) {
    return __atomic_load_n(&app->exit_requested, __ATOMIC_ACQUIRE) ||
           airbridge_exit_latch_requested(exit_latch);
}

static void airbridge_runtime_apply_startup(AirbridgeApp* app, AirbridgeExitLatch* exit_latch) {
    airbridge_operation_start(&app->operation, AirbridgeOperationUsbStart, furi_get_tick());
    const bool usb_ready = airbridge_relay_configure_usb(
        &app->relay, app->config.usb_profile_index, &app->config.usb_profile_index);
    airbridge_operation_end(&app->operation);
    if(!usb_ready) {
        airbridge_screens_show_fatal(
            app->screens,
            app->relay.admission_rejected ? "ENABLE FLIPPER USB FIRST" : "USB CONFIG ERROR");
        return;
    }
    if(airbridge_runtime_exit_requested(app, exit_latch)) return;
    airbridge_operation_start(&app->operation, AirbridgeOperationBleStart, furi_get_tick());
    const bool ble_ready = airbridge_ble_configure(
        &app->ble, &app->config.ble_identity, airbridge_relay_ble_event, &app->relay);
    airbridge_operation_end(&app->operation);
    if(!ble_ready) {
        airbridge_operation_start(&app->operation, AirbridgeOperationUsbStop, furi_get_tick());
        if(!airbridge_relay_restore_usb(&app->relay)) {
            FURI_LOG_E(TAG, "USB restore failed after BLE startup error");
        }
        airbridge_operation_end(&app->operation);
        airbridge_screens_show_fatal(app->screens, "BLE CONFIG ERROR");
    }
}

static void airbridge_runtime_handle_relay(AirbridgeApp* app, BridgeEvent* event) {
    if(event->type == EVENT_TYPE_RELAY) {
        airbridge_operation_start(
            &app->operation,
            event->to_ble ? AirbridgeOperationBleSend : AirbridgeOperationUsbSend,
            furi_get_tick());
        if(!event->to_ble) airbridge_ble_note_rx(&app->ble, event->tick);
        const AirbridgeRelayResult result = airbridge_relay_handle(
            &app->relay,
            &app->ble,
            event,
            airbridge_screens_current(app->screens),
            airbridge_screens_armed_transport(app->screens));
        airbridge_operation_end(&app->operation);
        if(result == AirbridgeRelayDeployRequestedUsb) {
            airbridge_screens_deploy_requested(app->screens, AirbridgeTypingTransportUsb);
        } else if(result == AirbridgeRelayDeployRequestedBle) {
            airbridge_screens_deploy_requested(app->screens, AirbridgeTypingTransportBle);
        } else if(result == AirbridgeRelayDeployNotArmed) {
            airbridge_screens_show_error(app->screens, "DEPLOY NOT ARMED");
        }
    } else if(event->type == EVENT_TYPE_USB) {
        airbridge_relay_set_usb_connected(&app->relay, event->to_ble);
        if(!event->to_ble) app->mouse.active = false;
    }
}

static void airbridge_runtime_service_screen(AirbridgeApp* app) {
    /* A fresh BLE link clears the deploy-accepted latch: only the 0x42 stream
     * start on the current link may arm the Waiting zombie exemption. */
    if(app->ble.link_generation != app->ble_generation_seen) {
        app->ble_generation_seen = app->ble.link_generation;
        airbridge_screens_note_ble_reconnect(app->screens);
    }
    /* Deferred BLE release-all from typing abort: ONE direct attempt per loop
     * iteration, never through the retry wrapper, and BEFORE the screen
     * dispatch so a pending release lands ahead of any new typing step. */
    const bool typing_release_ready = airbridge_typing_service_release(&app->typing);

    const AirbridgeScreen screen = airbridge_screens_current(app->screens);
    const AirbridgeTypingTransport armed = airbridge_screens_armed_transport(app->screens);
    if(screen == AirbridgeScreenBridge || screen == AirbridgeScreenSettings ||
       screen == AirbridgeScreenPasswords) {
        airbridge_operation_start(
            &app->operation, AirbridgeOperationBleReconnect, furi_get_tick());
        airbridge_ble_squatter_watchdog(&app->ble);
        airbridge_operation_end(&app->operation);
    } else if(screen == AirbridgeScreenPasswordTyping && typing_release_ready) {
        /* A short password must not finish before its TYPING frame reaches the LCD. */
        if(airbridge_ui_password_ready(app->ui, app->typing.generation) &&
           airbridge_typing_step(&app->typing))
            airbridge_screens_typing_complete(app->screens);
    } else if(screen == AirbridgeScreenTyping && typing_release_ready) {
        /* NO squatter kick on Typing: the HID host link is legitimate AND
         * never subscribes the serial TX CCCD, and typing runs 60-90 s. */
        if(airbridge_typing_step(&app->typing)) {
            airbridge_screens_typing_complete(app->screens);
            if(armed == AirbridgeTypingTransportBle) {
                /* The typing bond doubles as the browser's data bond; drop the
                 * HID link so the data client can connect for the fetch. */
                airbridge_ble_force_reconnect(&app->ble);
            }
        }
    } else if(screen == AirbridgeScreenStreaming) {
        /* NO squatter kick on Streaming: an active transfer is definitionally
         * a subscribed, legitimate client. */
        bool complete = false;
        if(armed == AirbridgeTypingTransportBle) {
            complete = airbridge_stream_step_ble(&app->stream);
        } else {
            complete = airbridge_stream_step_usb(&app->stream);
        }
        if(complete) {
            airbridge_screens_stream_complete(app->screens);
        }
    } else if(screen == AirbridgeScreenWaiting) {
        if(armed == AirbridgeTypingTransportBle) {
            const uint32_t now = furi_get_tick();
            if(now - airbridge_screens_waiting_pump_tick(app->screens) >= BLE_WAITING_PUMP_MS) {
                airbridge_screens_waiting_pump_mark(app->screens, now);
                /* Restarts advertising only from GAP idle: never disconnect a
                 * link or a numeric-comparison pairing to recover idle adv. */
                FURI_LOG_D(TAG, "BLE Waiting pump: restart adv if idle");
                airbridge_ble_ensure_serial_adv(&app->ble);
            }
            /* Squatter kick (Waiting): a bonded host HID daemon reconnects the
             * keyboard and sits on the link without subscribing TX, keeping
             * the Web Bluetooth picker empty. Past the deadline any still
             * unsubscribed central is safe to kick; pairing-ceremony time is
             * inside the 15 s budget. */
            airbridge_ble_squatter_watchdog(&app->ble);
            /* Waiting zombie-kick: a SUBSCRIBED zombie from a failed bootstrap
             * attempt survives the squatter watchdog and blocks advertising
             * forever. On Waiting the only legit link progress is the accepted
             * 0x42 deploy write. */
            if(app->ble.ble_profile_installed && app->ble.ble_connected &&
               !airbridge_screens_deploy_request_accepted(app->screens) &&
               now - app->ble.ble_connected_since >= BLE_WAITING_ZOMBIE_KICK_MS) {
                FURI_LOG_W(TAG, "BLE Waiting: kicking subscribed zombie link");
                airbridge_ble_force_reconnect(&app->ble);
            }
        }
    } else if(screen == AirbridgeScreenDone || screen == AirbridgeScreenError) {
        if(screen == AirbridgeScreenDone && app->ble.ble_profile_installed &&
           app->ble.ble_connected && app->stream.started_tick != 0 &&
           app->ble.ble_connected_since < app->stream.started_tick &&
           furi_get_tick() - app->stream.done_since >= BLE_DONE_ZOMBIE_GRACE_MS) {
            /* The deploy bootstrap must be disconnected by Done; a link whose
             * connect predates the stream start is that zombie by definition. */
            FURI_LOG_W(TAG, "BLE Done: kicking bootstrap zombie link");
            airbridge_ble_force_reconnect(&app->ble);
        }
        airbridge_operation_start(
            &app->operation, AirbridgeOperationBleReconnect, furi_get_tick());
        airbridge_ble_squatter_watchdog(&app->ble);
        airbridge_operation_end(&app->operation);
    }
    /* NO squatter kick on DeployPrompt: the pairing ceremony at the prompt can
     * exceed the kick window before the TX subscription lands. */
}

static void airbridge_runtime_update_ui(AirbridgeApp* app) {
    AirbridgeUiSnapshot snapshot = {
        .screen = airbridge_screens_current(app->screens),
        .deploy_transport = airbridge_screens_render_transport(app->screens),
        .ble_connected = app->ble.ble_connected,
        .identity_warning = app->config.identity_warning,
        .usb_profile_index = app->config.usb_profile_index,
        .typing_position = app->typing.position,
        .typing_total = app->typing.bootstrap_len,
        .typing_generation = app->typing.generation,
        .stream_sent = app->stream.sent,
        .stream_total = app->stream.total_len,
        .error = *airbridge_screens_error(app->screens),
    };
    snprintf(
        snapshot.ble_name, sizeof(snapshot.ble_name), "%s", app->config.ble_identity.device_name);
    airbridge_relay_metrics_snapshot(&app->relay, &snapshot.metrics);
    airbridge_utilities_snapshot(app, &snapshot);
    airbridge_ui_update(app->ui, &snapshot);
}

void airbridge_runtime_run(
    AirbridgeApp* app,
    NotificationApp* notifications,
    bool* startup_apply_pending,
    AirbridgeExitLatch* exit_latch) {
    app->running = true;
    airbridge_ui_resume_input(app->ui);
    while(app->running) {
        airbridge_ui_service_input(app->ui);
        if(airbridge_runtime_exit_requested(app, exit_latch)) break;
        airbridge_operation_start(
            &app->operation, AirbridgeOperationBleReconnect, furi_get_tick());
        airbridge_ble_service_pending(&app->ble);
        airbridge_operation_end(&app->operation);
        airbridge_ui_service_input(app->ui);
        if(airbridge_runtime_exit_requested(app, exit_latch)) break;

        BridgeEvent event;
        if(airbridge_relay_poll(&app->relay, &event, AIRBRIDGE_EXIT_POLL_INTERVAL_MS) ==
           FuriStatusOk) {
            airbridge_ui_service_input(app->ui);
            if(airbridge_runtime_exit_requested(app, exit_latch)) break;
            airbridge_runtime_handle_relay(app, &event);
        }
        if(airbridge_runtime_exit_requested(app, exit_latch)) break;
        if(*startup_apply_pending) {
            *startup_apply_pending = false;
            airbridge_runtime_apply_startup(app, exit_latch);
        }
        /* BACK may have arrived during a BLE wait. Consume it before another
         * typing step can emit a character after that operation returns. */
        airbridge_ui_service_input(app->ui);
        if(airbridge_runtime_exit_requested(app, exit_latch)) break;
        airbridge_runtime_service_screen(app);
        airbridge_utilities_mouse(app);
        airbridge_runtime_update_ui(app);

        if(furi_get_tick() - app->last_heartbeat >= 500) {
            notification_message(notifications, &sequence_blink_green_100);
            app->last_heartbeat = furi_get_tick();
        }
    }
    app->running = false;
}
