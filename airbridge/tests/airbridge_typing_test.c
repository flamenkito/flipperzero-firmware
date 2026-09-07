#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../applications_user/pocket_airbridge/airbridge_typing.h"
#include "../../applications_user/pocket_airbridge/airbridge_screens.h"
#include <furi_hal_usb_hid.h>

typedef enum {
    KeyPress,
    KeyRelease,
    KeyReleaseAll,
} KeyAction;

typedef struct {
    KeyAction action;
    uint16_t key;
} KeyEvent;

static struct {
    uint32_t tick;
    uint16_t held_key;
    KeyEvent events[16];
    size_t event_count;
    bool fail_release;
    bool fail_release_all;
    unsigned errors;
    char error[64];
} platform;

uint32_t furi_get_tick(void) {
    return platform.tick;
}

void furi_delay_ms(uint32_t milliseconds) {
    assert(platform.held_key != HID_KEYBOARD_NONE);
    assert(milliseconds >= 12 && milliseconds <= 19);
    platform.tick += milliseconds;
}

static void record_key(KeyAction action, uint16_t key) {
    assert(platform.event_count < COUNT_OF(platform.events));
    platform.events[platform.event_count++] = (KeyEvent){.action = action, .key = key};
}

bool airbridge_usb_kb_press(uint16_t key) {
    assert(platform.held_key == HID_KEYBOARD_NONE);
    record_key(KeyPress, key);
    platform.held_key = key;
    return true;
}

bool airbridge_usb_kb_release(uint16_t key) {
    assert(platform.held_key == key);
    record_key(KeyRelease, key);
    if(platform.fail_release) return false;
    platform.held_key = HID_KEYBOARD_NONE;
    return true;
}

bool airbridge_usb_kb_release_all(void) {
    record_key(KeyReleaseAll, HID_KEYBOARD_NONE);
    if(platform.fail_release_all) return false;
    platform.held_key = HID_KEYBOARD_NONE;
    return true;
}

/* These tests enter at typing_step with an already-loaded payload. Keep the
 * real loader and digest code linked, but reject unexpected storage access. */
File* storage_file_alloc(Storage* storage) {
    UNUSED(storage);
    abort();
}

void storage_file_free(File* file) {
    UNUSED(file);
    abort();
}

bool storage_file_open(
    File* file,
    const char* path,
    FS_AccessMode access_mode,
    FS_OpenMode open_mode) {
    UNUSED(file);
    UNUSED(path);
    UNUSED(access_mode);
    UNUSED(open_mode);
    abort();
}

bool storage_file_close(File* file) {
    UNUSED(file);
    abort();
}

uint64_t storage_file_size(File* file) {
    UNUSED(file);
    abort();
}

size_t storage_file_read(File* file, void* buffer, size_t size) {
    UNUSED(file);
    UNUSED(buffer);
    UNUSED(size);
    abort();
}

static void show_error(void* context, const char* message) {
    UNUSED(context);
    platform.errors++;
    snprintf(platform.error, sizeof(platform.error), "%s", message);
}

static AirbridgeTyping typing_init(const char* payload) {
    memset(&platform, 0, sizeof(platform));
    platform.tick = 100;
    char* bootstrap = malloc(strlen(payload) + 1);
    assert(bootstrap);
    strcpy(bootstrap, payload);
    return (AirbridgeTyping){
        .bootstrap = bootstrap,
        .bootstrap_len = strlen(payload),
        .show_error = show_error,
        .next_tick = platform.tick,
        .jitter_state = 1,
    };
}

static void assert_tap(size_t offset, uint16_t key, const AirbridgeTyping* typing) {
    assert(platform.event_count == offset + 2);
    assert(platform.events[offset].action == KeyPress);
    assert(platform.events[offset].key == key);
    assert(platform.events[offset + 1].action == KeyRelease);
    assert(platform.events[offset + 1].key == key);
    assert(platform.held_key == HID_KEYBOARD_NONE);
    assert(!typing->key_down);
    assert(platform.errors == 0);
}

static void test_step_releases_key_before_return(void) {
    AirbridgeTyping typing = typing_init("Aa");
    assert(!airbridge_typing_step(&typing));
    assert_tap(0, HID_KEYBOARD_A | KEY_MOD_LEFT_SHIFT, &typing);
    assert(typing.position == 1);

    /* The release delay remains between calls, with no key held during it. */
    assert(typing.next_tick - platform.tick >= 18);
    assert(typing.next_tick - platform.tick <= 25);
    assert(!airbridge_typing_step(&typing));
    platform.tick = typing.next_tick - 1;
    assert(!airbridge_typing_step(&typing));
    assert(platform.event_count == 2);
    platform.tick++;
    assert(!airbridge_typing_step(&typing));
    assert_tap(2, HID_KEYBOARD_A, &typing);
    assert(typing.position == 2 && !typing.enter_done);

    platform.tick = typing.next_tick;
    assert(!airbridge_typing_step(&typing));
    assert_tap(4, HID_KEYBOARD_RETURN, &typing);
    assert(typing.enter_done && !typing.enter_pending);
    platform.tick = typing.next_tick;
    assert(airbridge_typing_step(&typing));
    assert(platform.event_count == 6);
    airbridge_typing_deinit(&typing);
}

typedef struct {
    AirbridgeTyping typing;
    AirbridgeScreens* screens;
    unsigned aborts;
} ScreenFixture;

static bool screen_typing_start(void* context) {
    ScreenFixture* fixture = context;
    assert(!fixture->typing.key_down);
    /* Payload loading is outside the screen-policy test boundary. */
    return true;
}

static bool screen_typing_abort(void* context) {
    ScreenFixture* fixture = context;
    fixture->aborts++;
    return airbridge_typing_abort(&fixture->typing);
}

static bool screen_deploy_supported(void* context) {
    UNUSED(context);
    return true;
}

static void screen_show_error(void* context, const char* message) {
    ScreenFixture* fixture = context;
    show_error(NULL, message);
    airbridge_screens_show_error(fixture->screens, message);
}

static void screen_fixture_init(ScreenFixture* fixture, const char* payload) {
    *fixture = (ScreenFixture){.typing = typing_init(payload)};
    const AirbridgeScreenActions actions = {
        .typing_start = screen_typing_start,
        .typing_abort = screen_typing_abort,
        .deploy_supported = screen_deploy_supported,
    };
    fixture->screens = airbridge_screens_alloc(&actions, fixture);
    assert(fixture->screens);
    fixture->typing.show_error = screen_show_error;
    fixture->typing.error_context = fixture;
    assert(!airbridge_screens_handle_ui_intent(fixture->screens, AirbridgeUiIntentNext));
    assert(airbridge_screens_current(fixture->screens) == AirbridgeScreenDeployPrompt);
    assert(!airbridge_screens_handle_ui_intent(fixture->screens, AirbridgeUiIntentConfirm));
    assert(airbridge_screens_current(fixture->screens) == AirbridgeScreenTyping);
}

static void screen_fixture_free(ScreenFixture* fixture) {
    airbridge_screens_free(fixture->screens);
    airbridge_typing_deinit(&fixture->typing);
}

static void test_failed_release_aborts_and_shows_error(bool enter, bool abort_release_fails) {
    ScreenFixture fixture;
    screen_fixture_init(&fixture, "A");
    if(enter) fixture.typing.position = fixture.typing.bootstrap_len;
    const size_t position = fixture.typing.position;
    platform.fail_release = true;
    platform.fail_release_all = abort_release_fails;

    assert(!airbridge_typing_step(&fixture.typing));
    assert(platform.event_count == 3);
    assert(platform.events[0].action == KeyPress);
    assert(platform.events[1].action == KeyRelease);
    assert(platform.events[2].action == KeyReleaseAll);
    assert(
        platform.events[0].key ==
        (enter ? HID_KEYBOARD_RETURN : (HID_KEYBOARD_A | KEY_MOD_LEFT_SHIFT)));
    assert(platform.errors == 1);
    assert(strcmp(platform.error, "KEYBOARD SEND ERROR") == 0);
    assert(airbridge_screens_current(fixture.screens) == AirbridgeScreenError);
    assert(fixture.typing.position == position);
    assert(!fixture.typing.key_down && !fixture.typing.enter_done);
    assert(!fixture.typing.enter_pending);
    if(abort_release_fails) {
        /* A failed transport release is not evidence that the host released it. */
        assert(platform.held_key == platform.events[0].key);
        assert(strstr(airbridge_screens_error(fixture.screens)->action, "tap a key"));
    } else {
        assert(platform.held_key == HID_KEYBOARD_NONE);
    }
    screen_fixture_free(&fixture);
}

typedef struct {
    ScreenFixture screen;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool waiting;
    bool resume;
    bool back_queued;
} BlockedFixture;

static void await_change(BlockedFixture* fixture) {
    struct timespec deadline;
    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += 5;
    assert(pthread_cond_timedwait(&fixture->changed, &fixture->mutex, &deadline) == 0);
}

static void* blocked_worker(void* context) {
    BlockedFixture* fixture = context;
    assert(!airbridge_typing_step(&fixture->screen.typing));
    pthread_mutex_lock(&fixture->mutex);
    fixture->waiting = true;
    pthread_cond_broadcast(&fixture->changed);
    while(!fixture->resume)
        await_change(fixture);

    /* Model the runtime's resumption boundary, using the production BACK policy.
     * This does not exercise the GUI input queue or the BLE driver itself. */
    assert(fixture->back_queued);
    assert(!airbridge_screens_handle_ui_intent(fixture->screen.screens, AirbridgeUiIntentBack));
    if(airbridge_screens_current(fixture->screen.screens) == AirbridgeScreenTyping) {
        airbridge_typing_step(&fixture->screen.typing);
    }
    pthread_mutex_unlock(&fixture->mutex);
    return NULL;
}

static void test_back_after_blocked_interval_stops_next_tap(const char* payload) {
    BlockedFixture fixture = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    screen_fixture_init(&fixture.screen, payload);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, blocked_worker, &fixture) == 0);
    pthread_mutex_lock(&fixture.mutex);
    while(!fixture.waiting)
        await_change(&fixture);
    assert_tap(0, HID_KEYBOARD_A, &fixture.screen.typing);
    assert(airbridge_screens_current(fixture.screen.screens) == AirbridgeScreenTyping);
    fixture.back_queued = true;
    platform.tick += 60000;
    assert(platform.held_key == HID_KEYBOARD_NONE);
    assert(platform.event_count == 2);
    fixture.resume = true;
    pthread_cond_broadcast(&fixture.changed);
    pthread_mutex_unlock(&fixture.mutex);
    assert(pthread_join(worker, NULL) == 0);

    assert(fixture.screen.aborts == 1);
    assert(airbridge_screens_current(fixture.screen.screens) == AirbridgeScreenBridge);
    assert(platform.event_count == 2);
    assert(platform.held_key == HID_KEYBOARD_NONE);
    assert(!fixture.screen.typing.key_down && !fixture.screen.typing.enter_done);
    screen_fixture_free(&fixture.screen);
    pthread_cond_destroy(&fixture.changed);
    pthread_mutex_destroy(&fixture.mutex);
}

int main(void) {
    test_step_releases_key_before_return();
    test_failed_release_aborts_and_shows_error(false, false);
    test_failed_release_aborts_and_shows_error(true, false);
    test_failed_release_aborts_and_shows_error(false, true);
    test_failed_release_aborts_and_shows_error(true, true);
    test_back_after_blocked_interval_stops_next_tap("ab");
    test_back_after_blocked_interval_stops_next_tap("a");
    return 0;
}
