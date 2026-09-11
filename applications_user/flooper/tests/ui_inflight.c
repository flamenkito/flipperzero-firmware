#include "ui_fake.h"
#include "../flooper_app.h"

static pthread_mutex_t draw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t draw_changed = PTHREAD_COND_INITIALIZER;
static bool draw_entered, removing;
static pthread_t drawing;
static void blocked_draw(void) {
    pthread_mutex_lock(&draw_lock);
    draw_entered = true;
    pthread_cond_broadcast(&draw_changed);
    while(!removing)
        pthread_cond_wait(&draw_changed, &draw_lock);
    assert(ui_fake.removed == 0 && ui_fake.freed == 0);
    pthread_mutex_unlock(&draw_lock);
}
static void removing_draw(void) {
    pthread_mutex_lock(&draw_lock);
    assert(draw_entered && ui_fake.timer_freed == 1);
    removing = true;
    pthread_cond_broadcast(&draw_changed);
    pthread_mutex_unlock(&draw_lock);
}
static void* draw_thread(void* context) {
    (void)context;
    ui_fake_draw();
    return NULL;
}
static void inflight_step(void) {
    fake_sync();
    ui_fake_input(InputTypeLong, InputKeyBack);
    ui_fake.draw_hook = blocked_draw;
    ui_fake.remove_hook = removing_draw;
    assert(pthread_create(&drawing, NULL, draw_thread, NULL) == 0);
    pthread_mutex_lock(&draw_lock);
    while(!draw_entered)
        pthread_cond_wait(&draw_changed, &draw_lock);
    pthread_mutex_unlock(&draw_lock);
}
void test_ui_inflight(const char* root) {
    /* Given an in-flight draw blocked after its model copy; When main exits;
     * Then removal waits for draw before any callback-visible memory is freed. */
    fake_reset(1000, 0);
    fake.app_mode = true;
    ui_fake_reset();
    fake_read_document(root, "assets/flipper_tangos_pattern_v2_1.json");
    draw_entered = removing = false;
    fake.app_step = inflight_step;
    assert(flooper_app(NULL) == 0);
    assert(pthread_join(drawing, NULL) == 0);
    assert(ui_fake.removed == 1 && fake.joined);
    ui_cases++;
}
