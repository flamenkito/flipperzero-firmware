#include "airbridge_runtime.h"

#include <furi_hal_usb_airbridge.h>
#include <notification/notification_messages.h>

#define TAG "AirBridge"

static bool airbridge_runtime_typing_start(void* context) {
    AirbridgeApp* app = context;
    return airbridge_typing_start(&app->typing);
}

static bool airbridge_runtime_typing_abort(void* context) {
    AirbridgeApp* app = context;
    return airbridge_typing_abort(&app->typing);
}

static bool airbridge_runtime_stream_start(void* context) {
    AirbridgeApp* app = context;
    return airbridge_stream_start(&app->stream);
}

static void airbridge_runtime_stream_close(void* context) {
    AirbridgeApp* app = context;
    airbridge_stream_close(&app->stream);
}

static void airbridge_runtime_ble_reset(void* context) {
    AirbridgeApp* app = context;
    airbridge_ble_force_reconnect(&app->ble);
}

static bool airbridge_runtime_ble_profile_installed(void* context) {
    const AirbridgeApp* app = context;
    return app->ble.ble_profile_installed;
}

static bool airbridge_runtime_deploy_supported(void* context) {
    const AirbridgeApp* app = context;
    return furi_hal_usb_airbridge_profile_has_keyboard(app->config.usb_profile_index);
}

static void airbridge_runtime_exit(void* context) {
    AirbridgeApp* app = context;
    app->exit_requested = true;
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
    };
    return &actions;
}

static bool airbridge_runtime_exit_requested(
    const AirbridgeApp* app,
    AirbridgeExitLatch* exit_latch) {
    return app->exit_requested || airbridge_exit_latch_requested(exit_latch);
}

static void airbridge_runtime_apply_startup(AirbridgeApp* app) {
    const bool usb_ready = airbridge_relay_configure_usb(
        &app->relay, app->config.usb_profile_index, &app->config.usb_profile_index);
    if(!usb_ready) {
        airbridge_screens_show_fatal(app->screens, "USB CONFIG ERROR");
    } else if(!airbridge_ble_configure(&app->ble, &app->config.ble_identity)) {
        if(!airbridge_relay_restore_usb(&app->relay)) {
            FURI_LOG_E(TAG, "USB restore failed after BLE startup error");
        }
        airbridge_screens_show_fatal(app->screens, "BLE CONFIG ERROR");
    }
}

static void airbridge_runtime_handle_relay(AirbridgeApp* app, BridgeEvent* event) {
    if(event->type == EVENT_TYPE_RELAY) {
        if(!event->to_ble) airbridge_ble_note_rx(&app->ble, event->tick);
        const AirbridgeRelayResult result = airbridge_relay_handle(
            &app->relay, event, airbridge_screens_current(app->screens));
        if(result == AirbridgeRelayDeployRequested) {
            airbridge_screens_deploy_requested(app->screens);
        } else if(result == AirbridgeRelayDeployNotArmed) {
            airbridge_screens_show_error(app->screens, "DEPLOY NOT ARMED");
        }
    } else if(event->type == EVENT_TYPE_USB) {
        airbridge_relay_set_usb_connected(&app->relay, event->to_ble);
    }
}

static void airbridge_runtime_service_screen(AirbridgeApp* app) {
    const AirbridgeScreen screen = airbridge_screens_current(app->screens);
    if(screen == AirbridgeScreenBridge) {
        airbridge_ble_bridge_adv_watchdog(&app->ble);
        airbridge_ble_squatter_watchdog(&app->ble);
    } else if(screen == AirbridgeScreenTyping) {
        if(airbridge_typing_step(&app->typing)) {
            airbridge_screens_typing_complete(app->screens);
        }
    } else if(screen == AirbridgeScreenStreaming) {
        if(airbridge_stream_step_usb(&app->stream)) {
            airbridge_screens_stream_complete(app->screens);
        }
    } else if(screen == AirbridgeScreenDone || screen == AirbridgeScreenError) {
        airbridge_ble_squatter_watchdog(&app->ble);
    }
}

static void airbridge_runtime_update_ui(AirbridgeApp* app) {
    AirbridgeUiSnapshot snapshot = {
        .screen = airbridge_screens_current(app->screens),
        .ble_connected = app->ble.ble_connected,
        .identity_warning = app->config.identity_warning,
        .usb_profile_index = app->config.usb_profile_index,
        .typing_position = app->typing.position,
        .typing_total = app->typing.bootstrap_len,
        .stream_sent = app->stream.sent,
        .stream_total = app->stream.total_len,
        .error = *airbridge_screens_error(app->screens),
    };
    airbridge_relay_metrics_snapshot(&app->relay, &snapshot.metrics);
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
        airbridge_ble_service_pending(&app->ble);

        BridgeEvent event;
        if(airbridge_relay_poll(&app->relay, &event, AIRBRIDGE_EXIT_POLL_INTERVAL_MS) ==
           FuriStatusOk) {
            airbridge_runtime_handle_relay(app, &event);
        }
        if(airbridge_runtime_exit_requested(app, exit_latch)) break;
        if(*startup_apply_pending) {
            *startup_apply_pending = false;
            airbridge_runtime_apply_startup(app);
        }
        airbridge_runtime_service_screen(app);
        airbridge_runtime_update_ui(app);

        if(furi_get_tick() - app->last_heartbeat >= 500) {
            notification_message(notifications, &sequence_blink_green_100);
            app->last_heartbeat = furi_get_tick();
        }
    }
    app->running = false;
}
