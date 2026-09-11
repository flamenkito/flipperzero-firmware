#include "flooper_app.h"
#include "flooper_ui.h"
#include <gui/gui.h>
#include <stdio.h>

const FlooperCatalogEntry flooper_catalog[FLOOPER_CATALOG_COUNT] = {
    {"BULERIAS", "flipper_bulerias_pattern_v2_1.json"},
    {"TANGOS", "flipper_tangos_pattern_v2_1.json"},
};

typedef struct {
    InputEvent event;
    FlooperScreen screen;
} FlooperInput;
typedef struct {
    Gui* gui;
    ViewPort* viewport;
    FuriMessageQueue* input;
    FuriTimer* timer;
    FlooperUi* ui;
    FlooperPlayer* player;
    uint32_t started;
    FlooperScreen screen;
    uint8_t selected;
    bool registered, exit_requested, pending_load;
} FlooperApp;

static bool catalog_valid(void) {
    for(size_t i = 0; i < FLOOPER_CATALOG_COUNT; ++i) {
        const FlooperCatalogEntry* entry = &flooper_catalog[i];
        if(!entry->display_name || !entry->filename || !entry->display_name[0] ||
           !entry->filename[0])
            return false;
        if(!memchr(entry->filename, 0, FLOOPER_CATALOG_FILENAME_MAX) ||
           strchr(entry->filename, '/') || strchr(entry->filename, '\\') ||
           strstr(entry->filename, ".."))
            return false;
        if(!memchr(entry->display_name, 0, FLOOPER_ID_BYTES)) return false;
    }
    return true;
}

static void input_callback(InputEvent* event, void* context) {
    FlooperApp* app = context;
    if(event->type == InputTypeLong && event->key == InputKeyBack) {
        if(app->player) flooper_player_request_exit(app->player);
        __atomic_store_n(&app->exit_requested, true, __ATOMIC_RELEASE);
        return;
    }
    FlooperScreen screen = __atomic_load_n(&app->screen, __ATOMIC_ACQUIRE);
    if(screen == FlooperSplash) return;
    if((event->type == InputTypeShort &&
        (event->key == InputKeyUp || event->key == InputKeyDown || event->key == InputKeyOk)) ||
       (event->type == InputTypeLong && event->key == InputKeyOk)) {
        const FlooperInput input = {.event = *event, .screen = screen};
        furi_message_queue_put(app->input, &input, 0);
    }
}

static void refresh_callback(void* context) {
    FlooperApp* app = context;
    const FlooperInput tick = {.event.type = InputTypeMAX};
    furi_message_queue_put(app->input, &tick, 0);
}

static void close_app(FlooperApp* app) {
    if(app->timer) {
        furi_timer_stop(app->timer);
        furi_timer_free(app->timer);
        app->timer = NULL;
    }
    if(app->viewport) {
        view_port_enabled_set(app->viewport, false);
        if(app->registered) gui_remove_view_port(app->gui, app->viewport);
        app->registered = false;
        view_port_free(app->viewport);
        app->viewport = NULL;
    }
    if(app->input) furi_message_queue_free(app->input);
    app->input = NULL;
    if(app->ui) {
        if(app->ui->mutex) furi_mutex_free(app->ui->mutex);
        free(app->ui);
        app->ui = NULL;
    }
    if(app->player) flooper_player_free(app->player);
    app->player = NULL;
    if(app->gui) furi_record_close(RECORD_GUI);
    app->gui = NULL;
}

static void route_input(FlooperApp* app, const FlooperInput* input) {
    if(input->screen != app->screen || input->event.type == InputTypeMAX) return;
    const InputEvent* event = &input->event;
    switch(app->screen) {
    case FlooperSplash:
        break;
    case FlooperSelector:
        if(event->type != InputTypeShort) break;
        switch(event->key) {
        case InputKeyUp:
            app->selected = (app->selected + FLOOPER_CATALOG_COUNT - 1) % FLOOPER_CATALOG_COUNT;
            break;
        case InputKeyDown:
            app->selected = (app->selected + 1) % FLOOPER_CATALOG_COUNT;
            break;
        case InputKeyOk:
            app->pending_load = true;
            __atomic_store_n(&app->screen, FlooperPlayback, __ATOMIC_RELEASE);
            break;
        default:
            break;
        }
        break;
    case FlooperPlayback:
        if(event->key == InputKeyOk && !app->pending_load) {
            if(event->type == InputTypeShort)
                flooper_player_send(app->player, (FlooperCommand){.type = FlooperCommandToggle});
            else if(event->type == InputTypeLong)
                flooper_player_send(app->player, (FlooperCommand){.type = FlooperCommandRestart});
        }
        break;
    }
}

static void update_model(FlooperApp* app) {
    uint32_t now = furi_get_tick();
    if(app->screen == FlooperSplash && (uint32_t)(now - app->started) >= furi_ms_to_ticks(1000))
        __atomic_store_n(&app->screen, FlooperSelector, __ATOMIC_RELEASE);
    FlooperSnapshot snapshot;
    bool copied = flooper_player_snapshot(app->player, &snapshot);
    bool loaded = false;
    if(app->pending_load && copied &&
       (snapshot.state == FlooperStatePaused || snapshot.state == FlooperStatePatternError)) {
        loaded = flooper_player_send(
            app->player,
            (FlooperCommand){.type = FlooperCommandLoadSelected, .selected_index = app->selected});
        if(loaded) app->pending_load = false;
    }
    furi_mutex_acquire(app->ui->mutex, FuriWaitForever);
    FlooperUiModel* m = &app->ui->model;
    m->screen = app->screen;
    snprintf(
        m->selected_name,
        sizeof(m->selected_name),
        "%s",
        flooper_catalog[app->selected].display_name);
    if(loaded || app->pending_load) {
        m->snapshot = (FlooperSnapshot){.state = FlooperStateLoading};
        m->last_label = 0;
        m->contact = false;
        if(loaded) m->minimum_generation = snapshot.generation + 1;
    }
    flooper_ui_tick(m, copied && !app->pending_load ? &snapshot : NULL, now);
    furi_mutex_release(app->ui->mutex);
    view_port_update(app->viewport);
}

int32_t flooper_app(void* context) {
    (void)context;
    FlooperApp app = {.started = furi_get_tick(), .screen = FlooperSplash};
    int32_t result = -1;
    app.gui = furi_record_open(RECORD_GUI);
    if(!catalog_valid()) goto done;
    /* Scheduler-locked admission covers model, ViewPort/mutex, queue and timer;
     * local allocators abort on OOM. No blocking API belongs inside this span. */
    int32_t lock = furi_kernel_lock();
    if(memmgr_heap_get_max_free_block() >= 8192) {
        app.ui = malloc(sizeof(*app.ui));
        if(app.ui) {
            memset(app.ui, 0, sizeof(*app.ui));
            app.ui->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
            app.viewport = view_port_alloc();
            app.input = furi_message_queue_alloc(8, sizeof(FlooperInput));
            app.timer = furi_timer_alloc(refresh_callback, FuriTimerTypePeriodic, &app);
        }
    }
    furi_kernel_restore_lock(lock);
    if(!app.ui || !app.ui->mutex || !app.viewport || !app.input || !app.timer) goto done;
    app.player = flooper_player_alloc(0);
    if(!app.player) goto done;
    view_port_draw_callback_set(app.viewport, flooper_ui_draw, app.ui);
    view_port_input_callback_set(app.viewport, input_callback, &app);
    gui_add_view_port(app.gui, app.viewport, GuiLayerFullscreen);
    app.registered = true;
    uint32_t cadence = furi_kernel_get_tick_frequency() / 100;
    if(!cadence) cadence = 1;
    if(furi_timer_start(app.timer, cadence) != FuriStatusOk) goto done;
    while(!__atomic_load_n(&app.exit_requested, __ATOMIC_ACQUIRE)) {
        update_model(&app);
        FlooperInput input;
        if(furi_message_queue_get(app.input, &input, cadence) == FuriStatusOk &&
           !__atomic_load_n(&app.exit_requested, __ATOMIC_ACQUIRE))
            route_input(&app, &input);
    }
    result = 0;
done:
    close_app(&app);
    return result;
}
