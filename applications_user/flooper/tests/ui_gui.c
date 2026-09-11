#include "ui_fake.h"

UiFake ui_fake;
static pthread_mutex_t gui_lock = PTHREAD_MUTEX_INITIALIZER;
struct ViewPort {
    ViewPortDrawCallback draw;
    ViewPortInputCallback input;
    void* draw_context;
    void* input_context;
    bool enabled;
};
struct FuriTimer {
    FuriTimerCallback callback;
    void* context;
    bool running;
};

void ui_fake_reset(void) {
    assert(!ui_fake.viewport && !ui_fake.timer && fake.gui_records == 0);
    memset(&ui_fake, 0, sizeof(ui_fake));
}
ViewPort* view_port_alloc(void) {
    ViewPort* viewport = calloc(1, sizeof(*viewport));
    assert(viewport);
    viewport->enabled = true;
    return viewport;
}
void view_port_free(ViewPort* viewport) {
    assert(!ui_fake.viewport && !viewport->enabled && fake_locks == 0);
    ui_fake.freed++;
    free(viewport);
}
void view_port_enabled_set(ViewPort* viewport, bool enabled) {
    assert(fake_locks == 0 && !enabled && !ui_fake.timer);
    viewport->enabled = enabled;
    ui_fake.disabled++;
}
void view_port_update(ViewPort* viewport) {
    assert(viewport && fake_locks == 0);
    ui_fake.updates++;
}
void view_port_draw_callback_set(ViewPort* v, ViewPortDrawCallback cb, void* ctx) {
    v->draw = cb;
    v->draw_context = ctx;
}
void view_port_input_callback_set(ViewPort* v, ViewPortInputCallback cb, void* ctx) {
    v->input = cb;
    v->input_context = ctx;
    ui_fake.app_context = ctx;
}
void gui_add_view_port(Gui* gui, ViewPort* viewport, GuiLayer layer) {
    assert(gui && layer == GuiLayerFullscreen && !ui_fake.viewport && !fake_locks);
    ui_fake.viewport = viewport;
    ui_fake.added++;
}
void gui_remove_view_port(Gui* gui, ViewPort* viewport) {
    assert(gui && !fake_locks && !ui_fake.timer);
    if(ui_fake.remove_hook) ui_fake.remove_hook();
    pthread_mutex_lock(&gui_lock);
    assert(viewport == ui_fake.viewport && !viewport->enabled);
    ui_fake.viewport = NULL;
    ui_fake.removed++;
    pthread_mutex_unlock(&gui_lock);
}
void ui_fake_draw(void) {
    pthread_mutex_lock(&gui_lock);
    assert(ui_fake.viewport);
    ui_fake.viewport->draw(&ui_fake.canvas, ui_fake.viewport->draw_context);
    ui_capture();
    pthread_mutex_unlock(&gui_lock);
}
void ui_fake_input(InputType type, InputKey key) {
    pthread_mutex_lock(&gui_lock);
    assert(ui_fake.viewport);
    InputEvent event = {.type = type, .key = key};
    ui_fake.viewport->input(&event, ui_fake.viewport->input_context);
    pthread_mutex_unlock(&gui_lock);
}
FuriTimer* furi_timer_alloc(FuriTimerCallback callback, FuriTimerType type, void* context) {
    assert(type == FuriTimerTypePeriodic && !ui_fake.timer);
    FuriTimer* timer = calloc(1, sizeof(*timer));
    assert(timer);
    *timer = (FuriTimer){.callback = callback, .context = context};
    ui_fake.timer = timer;
    return timer;
}
FuriStatus furi_timer_start(FuriTimer* timer, uint32_t ticks) {
    assert(ticks && ticks <= fake.hz / 50);
    timer->running = true;
    return FuriStatusOk;
}
FuriStatus furi_timer_stop(FuriTimer* timer) {
    assert(timer == ui_fake.timer);
    timer->running = false;
    ui_fake.timer_stopped++;
    return FuriStatusOk;
}
void furi_timer_free(FuriTimer* timer) {
    assert(!timer->running && timer == ui_fake.timer);
    ui_fake.timer_freed++;
    ui_fake.timer = NULL;
    free(timer);
}
void ui_fake_tick(void) {
    if(ui_fake.timer && ui_fake.timer->running) ui_fake.timer->callback(ui_fake.timer->context);
}
