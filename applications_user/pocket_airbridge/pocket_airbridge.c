#include <furi.h>
#include <stdlib.h>
#include <string.h>
#include <furi_hal_usb.h>

#include <bt/bt_service/bt.h>

#include "airbridge_app_i.h"
#include "airbridge_exit_contract.h"
#include "airbridge_runtime.h"

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

    app->screens =
        airbridge_screens_alloc(airbridge_runtime_screen_actions(), app);
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
        &app->typing, app->storage, airbridge_screens_show_error, app->screens);
    airbridge_stream_init(
        &app->stream, app->storage, airbridge_screens_show_error, app->screens);
    airbridge_ui_init_view(app->ui);
    airbridge_ui_open_gui(app->ui);
    airbridge_ui_add_view(app->ui);
    airbridge_config_load(&app->config, app->storage);
    app->relay.usb_mode_prev = furi_hal_usb_get_config();
}

static void airbridge_app_free(AirbridgeApp* app) {
    airbridge_ui_free_view(app->ui);
    airbridge_ui_free(app->ui);
    airbridge_relay_deinit(&app->relay);
    airbridge_typing_deinit(&app->typing);
    airbridge_screens_free(app->screens);
    free(app);
}

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);
    AirbridgeApp* app = airbridge_app_alloc();
    if(app == NULL) return -1;
    airbridge_app_init_services(app);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    FuriThread* app_thread = furi_thread_get_current();
    /* Reset only after initialization and while detached. The callback ignores
     * context because callback/context publication is not atomic. */
    airbridge_exit_latch_reset(&app_exit_latch);
    furi_thread_set_signal_callback(app_thread, app_signal_callback, NULL);

    bool startup_apply_pending = true;
run_app:
    airbridge_runtime_run(app, notifications, &startup_apply_pending, &app_exit_latch);

    Bt* bt = NULL;
    AirbridgeExitContract exit_contract = airbridge_exit_contract_initial();
    if(!airbridge_exit_contract_record_detach(
           &exit_contract, airbridge_ble_prepare_restore(&app->ble, &bt))) {
        FURI_LOG_E(TAG, "BLE callback detach timed out; retaining app ownership");
        app->exit_requested = false;
        airbridge_exit_latch_reset(&app_exit_latch);
        goto run_app;
    }

    furi_check(airbridge_exit_contract_may_destroy(&exit_contract));
    furi_thread_set_signal_callback(app_thread, NULL, NULL);
    airbridge_stream_close(&app->stream);
    (void)airbridge_typing_abort(&app->typing);
    if(!airbridge_relay_restore_usb(&app->relay)) {
        FURI_LOG_E(TAG, "Failed to queue USB restore at teardown");
    }
    airbridge_ui_remove_view(app->ui);
    airbridge_ui_close_gui();
    furi_record_close(RECORD_NOTIFICATION);
    airbridge_typing_release_file(&app->typing);
    airbridge_stream_deinit(&app->stream);
    furi_record_close(RECORD_STORAGE);
    airbridge_app_free(app);
    airbridge_ble_queue_restore(bt);
    return 0;
}
