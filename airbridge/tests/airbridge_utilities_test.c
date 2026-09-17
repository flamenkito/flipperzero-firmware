#include <assert.h>
#include <string.h>
#include "../../applications_user/pocket_airbridge/airbridge_utilities.h"

static char saved_setting;
static char pending_setting;
static bool fail_write;
static bool fail_sync;
static bool fail_rename;
static bool connected = true;
static bool keyboard = true;
static bool file_live;
static int file_kind;
static uint32_t tick;
static const char* password_file = "work=secret=value\nother= second ";
static AirbridgeScreen screen = AirbridgeScreenBridge;
static unsigned mouse_reports;
static int8_t last_x;
static int8_t last_y;
static unsigned typing_calls;

File* storage_file_alloc(Storage* storage) {
    (void)storage;
    assert(!file_live);
    file_live = true;
    return (File*)&file_kind;
}
void storage_file_free(File* file) {
    (void)file;
    assert(file_live);
    file_live = false;
}
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    (void)file;
    (void)access;
    (void)mode;
    if(strcmp(path, APP_DATA_PATH("mouse_mover")) == 0) {
        file_kind = 1;
        return saved_setting != 0;
    }
    if(strcmp(path, APP_DATA_PATH("mouse_mover.tmp")) == 0) {
        file_kind = 2;
        return true;
    }
    assert(strcmp(path, APP_DATA_PATH("passwords.txt")) == 0);
    file_kind = 3;
    return password_file != NULL;
}
bool storage_file_close(File* file) {
    (void)file;
    return true;
}
uint64_t storage_file_size(File* file) {
    (void)file;
    return file_kind == 3 ? strlen(password_file) : 1;
}
size_t storage_file_read(File* file, void* buffer, size_t size) {
    (void)file;
    memcpy(buffer, file_kind == 3 ? password_file : &saved_setting, size);
    return size;
}
size_t storage_file_write(File* file, const void* buffer, size_t size) {
    (void)file;
    assert(file_kind == 2 && size == 1);
    if(fail_write) return 0;
    pending_setting = *(const char*)buffer;
    return 1;
}
bool storage_file_sync(File* file) {
    (void)file;
    return !fail_sync;
}
FS_Error storage_common_mkdir(Storage* storage, const char* path) {
    (void)storage;
    assert(strcmp(path, STORAGE_APP_DATA_PATH_PREFIX) == 0);
    return FSE_OK;
}
FS_Error storage_common_rename(Storage* storage, const char* old_path, const char* new_path) {
    (void)storage;
    assert(strcmp(old_path, APP_DATA_PATH("mouse_mover.tmp")) == 0);
    assert(strcmp(new_path, APP_DATA_PATH("mouse_mover")) == 0);
    if(fail_rename) return FSE_INTERNAL;
    saved_setting = pending_setting;
    return FSE_OK;
}
uint32_t furi_get_tick(void) {
    return tick;
}
bool airbridge_usb_vendor_is_connected(void) {
    return connected;
}
bool airbridge_usb_profile_has_keyboard(uint8_t index) {
    (void)index;
    return keyboard;
}
bool airbridge_usb_mouse_move(int8_t x, int8_t y) {
    last_x = x;
    last_y = y;
    mouse_reports++;
    return true;
}
AirbridgeScreen airbridge_screens_current(const AirbridgeScreens* screens) {
    (void)screens;
    return screen;
}
bool airbridge_typing_password_start(AirbridgeTyping* typing, const char* value) {
    (void)typing;
    assert(strcmp(value, "secret=value") == 0);
    typing_calls++;
    return true;
}

int main(void) {
    AirbridgeApp app = {0};
    airbridge_utilities_load(&app);
    assert(!app.mouse_enabled);
    airbridge_utilities_confirm(&app, AirbridgeScreenSettings);
    assert(app.mouse_enabled && saved_setting == '1');
    AirbridgeApp restarted = {0};
    airbridge_utilities_load(&restarted);
    assert(restarted.mouse_enabled);
    fail_write = true;
    airbridge_utilities_confirm(&app, AirbridgeScreenSettings);
    assert(app.mouse_enabled && app.menu_status[0]);
    fail_write = false;
    fail_sync = true;
    airbridge_utilities_confirm(&app, AirbridgeScreenSettings);
    assert(app.mouse_enabled && saved_setting == '1');
    fail_sync = false;
    fail_rename = true;
    airbridge_utilities_confirm(&app, AirbridgeScreenSettings);
    assert(app.mouse_enabled && saved_setting == '1');
    fail_rename = false;
    airbridge_utilities_mouse(&app);
    for(unsigned i = 0; i < 4; i++) {
        static const int8_t expected[4][2] = {{2, 0}, {0, 2}, {-2, 0}, {0, -2}};
        tick += 60000;
        airbridge_utilities_mouse(&app);
        assert(mouse_reports == i + 1 && last_x == expected[i][0] && last_y == expected[i][1]);
    }
    screen = AirbridgeScreenPasswords;
    tick += 60000;
    airbridge_utilities_mouse(&app);
    assert(mouse_reports == 4);
    screen = AirbridgeScreenSettings;
    tick += 60000;
    airbridge_utilities_mouse(&app);
    assert(mouse_reports == 4);
    tick += 60000;
    airbridge_utilities_mouse(&app);
    assert(mouse_reports == 5 && last_x == 2 && last_y == 0);
    airbridge_utilities_confirm(&app, AirbridgeScreenSettings);
    assert(!app.mouse_enabled && saved_setting == '0' && !app.menu_status[0]);
    airbridge_utilities_enter(&app, AirbridgeScreenPasswords);
    assert(app.passwords.count == 2);
    airbridge_utilities_move(&app, -1);
    assert(app.password_selected == 1);
    airbridge_utilities_move(&app, 1);
    assert(app.password_selected == 0);
    AirbridgeUiSnapshot snapshot = {.screen = AirbridgeScreenPasswords};
    airbridge_utilities_snapshot(&app, &snapshot);
    assert(strcmp(snapshot.password_names[0], "work") == 0);
    assert(strcmp(snapshot.password_names[1], "other") == 0);
    connected = false;
    assert(!airbridge_utilities_confirm(&app, AirbridgeScreenPasswords) && typing_calls == 0);
    connected = true;
    keyboard = false;
    assert(!airbridge_utilities_confirm(&app, AirbridgeScreenPasswords) && typing_calls == 0);
    keyboard = true;
    assert(airbridge_utilities_confirm(&app, AirbridgeScreenPasswords) && typing_calls == 1);
    airbridge_utilities_enter(&app, AirbridgeScreenSettings);
    assert(app.passwords.count == 0 && app.passwords.entries[0].value[0] == 0);
    password_file = "bad";
    airbridge_utilities_enter(&app, AirbridgeScreenPasswords);
    assert(!app.passwords.count && app.menu_status[0]);
    password_file = NULL;
    airbridge_utilities_enter(&app, AirbridgeScreenPasswords);
    assert(!app.passwords.count && app.menu_status[0]);
    assert(!file_live);
    return 0;
}
