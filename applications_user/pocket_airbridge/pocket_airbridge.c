#include <furi.h>
#include <stdlib.h>
#include <string.h>
#include <furi_hal_usb.h>

#include <bt/bt_service/bt.h>

#include "airbridge_app_i.h"
#include "airbridge_exit_contract.h"
#include "airbridge_lifecycle.h"
#include "airbridge_runtime.h"
#include "airbridge_utilities.h"

#define TAG "AirBridge"

/* Static storage outlives app-owned state, so an in-flight loader callback can
 * safely finish after teardown without touching the app or relay queue. */
static AirbridgeExitLatch app_exit_latch = AIRBRIDGE_EXIT_LATCH_INITIALIZER;

static bool app_signal_callback(uint32_t signal, void* arg, void* context) {
    UNUSED(arg);
    UNUSED(context);
    if(signal != FuriSignalExit) return false;
    airbridge_exit_latch_request(&app_exit_latch);
    return true;
}

static AirbridgeApp* airbridge_app_alloc(void) {
    AirbridgeApp* app = malloc(sizeof(*app));
    if(app == NULL) return NULL;
    memset(app, 0, sizeof(*app));
    app->last_heartbeat = furi_get_tick();
    airbridge_relay_init(&app->relay);

    app->screens = airbridge_screens_alloc(airbridge_runtime_screen_actions(), app);
    if(app->screens == NULL) {
        airbridge_relay_deinit(&app->relay);
        free(app);
        return NULL;
    }
    app->ui = airbridge_ui_alloc(airbridge_screens_handle_ui_intent, app->screens);
    if(app->ui == NULL) {
        airbridge_screens_free(app->screens);
        airbridge_relay_deinit(&app->relay);
        free(app);
        return NULL;
    }
    return app;
}

static void airbridge_app_init_services(AirbridgeApp* app) {
    app->storage = furi_record_open(RECORD_STORAGE);
    airbridge_typing_init(
        &app->typing, app->storage, &app->ble, airbridge_screens_show_error, app->screens);
    airbridge_stream_init(
        &app->stream, app->storage, &app->ble, airbridge_screens_show_error, app->screens);
    airbridge_ui_init_view(app->ui);
    airbridge_ui_open_gui(app->ui);
    airbridge_ui_add_view(app->ui);
    airbridge_config_load(&app->config, app->storage);
    airbridge_utilities_load(app);
}

static void airbridge_app_free(AirbridgeApp* app) {
    airbridge_passwords_clear(&app->passwords);
    airbridge_ui_free_view(app->ui);
    airbridge_ui_free(app->ui);
    airbridge_relay_deinit(&app->relay);
    airbridge_typing_deinit(&app->typing);
    airbridge_screens_free(app->screens);
    free(app);
}

static void airbridge_app_show_closing(void* context) {
    AirbridgeApp* app = context;
    (void)airbridge_typing_abort(&app->typing);
    airbridge_ui_show_closing(app->ui);
    airbridge_stream_close(&app->stream);
}

static bool airbridge_app_restore_usb(void* context) {
    return airbridge_relay_restore_usb(&((AirbridgeApp*)context)->relay);
}

static bool airbridge_app_restore_ble(void* context) {
    return airbridge_ble_restore(&((AirbridgeApp*)context)->ble);
}

static uint32_t airbridge_app_now(void* context) {
    UNUSED(context);
    return furi_get_tick();
}

static void airbridge_app_wait(void* context) {
    UNUSED(context);
    furi_delay_ms(100);
}

static int32_t airbridge_app_worker(void* context) {
    AirbridgeApp* app = context;
    bool startup_apply_pending = true;
    airbridge_runtime_run(app, app->notifications, &startup_apply_pending, &app_exit_latch);

    AirbridgeExitContract exit_contract = airbridge_exit_contract_initial();
    const AirbridgeLifecycleOps lifecycle = {
        .context = app,
        .show_closing = airbridge_app_show_closing,
        .restore_usb = airbridge_app_restore_usb,
        .restore_ble = airbridge_app_restore_ble,
        .now = airbridge_app_now,
        .wait = airbridge_app_wait,
    };
    airbridge_lifecycle_close(&lifecycle, &app->operation, &exit_contract);
    furi_check(airbridge_exit_contract_may_destroy(&exit_contract));
    return 0;
}

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);
    AirbridgeApp* app = airbridge_app_alloc();
    if(app == NULL) return -1;
    airbridge_app_init_services(app);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    Bt* monitor_bt = furi_record_open(RECORD_BT);
    FuriThread* app_thread = furi_thread_get_current();
    airbridge_exit_latch_reset(&app_exit_latch);
    furi_thread_set_signal_callback(app_thread, app_signal_callback, NULL);

    FuriThread* worker = furi_thread_alloc_ex("AirBridgeIO", 4096, airbridge_app_worker, app);
    furi_thread_start(worker);
    bool reported_stall = false;
    while(furi_thread_get_state(worker) != FuriThreadStateStopped) {
        const AirbridgeOperationStatus status = airbridge_operation_observe(
            &app->operation, furi_get_tick(), bt_pairing_in_progress(monitor_bt));
        airbridge_ui_set_operation_status(app->ui, status);
        if(status.stalled && !reported_stall) {
            reported_stall = true;
            FURI_LOG_E(TAG, "Operation %u stalled; retaining app until cleanup", status.operation);
            airbridge_exit_latch_request(&app_exit_latch);
        }
        furi_delay_ms(25);
    }
    furi_thread_join(worker);
    furi_thread_free(worker);
    airbridge_usb_free();
    furi_thread_set_signal_callback(app_thread, NULL, NULL);
    airbridge_ui_remove_view(app->ui);
    airbridge_ui_close_gui();
    furi_record_close(RECORD_NOTIFICATION);
    airbridge_typing_release_file(&app->typing);
    airbridge_stream_deinit(&app->stream);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_BT);
    airbridge_app_free(app);
    return 0;
}
