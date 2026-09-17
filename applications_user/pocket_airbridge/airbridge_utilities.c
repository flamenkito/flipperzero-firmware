#include "airbridge_utilities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOUSE_SETTING      APP_DATA_PATH("mouse_mover")
#define MOUSE_SETTING_TEMP APP_DATA_PATH("mouse_mover.tmp")

void airbridge_utilities_load(AirbridgeApp* app) {
    app->mouse_enabled = false;
    File* file = storage_file_alloc(app->storage);
    char value = '0';
    if(storage_file_open(file, MOUSE_SETTING, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_size(file) == 1 && storage_file_read(file, &value, 1) == 1)
        app->mouse_enabled = value == '1';
    storage_file_close(file);
    storage_file_free(file);
}

void airbridge_utilities_enter(void* context, AirbridgeScreen screen) {
    AirbridgeApp* app = context;
    airbridge_passwords_clear(&app->passwords);
    app->password_selected = 0;
    app->menu_status[0] = '\0';
    if(screen != AirbridgeScreenPasswords) return;
    File* file = storage_file_alloc(app->storage);
    const char* error = NULL;
    char* data = NULL;
    size_t size = 0;
    if(!storage_file_open(file, APP_DATA_PATH("passwords.txt"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        error = "Add passwords.txt on SD";
    } else if(storage_file_size(file) > AIRBRIDGE_PASSWORD_FILE_MAX) {
        error = "File too large (8 KiB max)";
    } else {
        size = storage_file_size(file);
        data = malloc(size + 1);
        if(!data) {
            error = "Not enough memory";
        } else if(storage_file_read(file, data, size) != size) {
            error = "Password file read failed";
        } else if(!airbridge_passwords_parse(&app->passwords, data, size)) {
            error = "Invalid password file";
        } else if(!app->passwords.count) {
            error = "No password entries";
        }
    }
    if(data) {
        volatile char* bytes = data;
        for(size_t i = 0; i < size; i++)
            bytes[i] = 0;
        free(data);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(error) snprintf(app->menu_status, sizeof(app->menu_status), "%s", error);
}

void airbridge_utilities_move(void* context, int direction) {
    AirbridgeApp* app = context;
    if(!app->passwords.count) return;
    app->password_selected =
        (app->password_selected + direction + app->passwords.count) % app->passwords.count;
    app->menu_status[0] = '\0';
}

bool airbridge_utilities_confirm(void* context, AirbridgeScreen screen) {
    AirbridgeApp* app = context;
    if(screen == AirbridgeScreenSettings) {
        storage_common_mkdir(app->storage, STORAGE_APP_DATA_PATH_PREFIX);
        File* file = storage_file_alloc(app->storage);
        const char value = app->mouse_enabled ? '0' : '1';
        bool saved = storage_file_open(file, MOUSE_SETTING_TEMP, FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
                     storage_file_write(file, &value, 1) == 1 && storage_file_sync(file);
        storage_file_close(file);
        storage_file_free(file);
        if(saved)
            saved = storage_common_rename(app->storage, MOUSE_SETTING_TEMP, MOUSE_SETTING) ==
                    FSE_OK;
        if(saved) {
            app->mouse_enabled = !app->mouse_enabled;
            app->mouse.active = false;
            app->menu_status[0] = '\0';
        } else {
            snprintf(app->menu_status, sizeof(app->menu_status), "Could not save setting");
        }
        return false;
    }
    if(screen != AirbridgeScreenPasswords || !app->passwords.count) return false;
    if(!airbridge_usb_profile_has_keyboard(app->config.usb_profile_index)) {
        snprintf(app->menu_status, sizeof(app->menu_status), "USB profile cannot type");
        return false;
    }
    if(!airbridge_usb_vendor_is_connected()) {
        snprintf(app->menu_status, sizeof(app->menu_status), "Connect USB to type");
        return false;
    }
    if(!airbridge_typing_password_start(
           &app->typing, app->passwords.entries[app->password_selected].value)) {
        snprintf(app->menu_status, sizeof(app->menu_status), "Could not start typing");
        return false;
    }
    app->menu_status[0] = '\0';
    return true;
}

void airbridge_utilities_mouse(AirbridgeApp* app) {
    AirbridgeScreen screen = airbridge_screens_current(app->screens);
    const bool active = app->mouse_enabled && airbridge_usb_vendor_is_connected() &&
                        (screen == AirbridgeScreenBridge || screen == AirbridgeScreenSettings);
    if(!airbridge_mouse_due(&app->mouse, active, furi_get_tick())) return;
    static const int8_t steps[4][2] = {{2, 0}, {0, 2}, {-2, 0}, {0, -2}};
    if(airbridge_usb_mouse_move(steps[app->mouse.step][0], steps[app->mouse.step][1])) {
        app->mouse.sent++;
        app->mouse.step = (app->mouse.step + 1) % 4;
    } else {
        app->mouse.failed++;
    }
}

void airbridge_utilities_snapshot(AirbridgeApp* app, AirbridgeUiSnapshot* snapshot) {
    if(snapshot->screen != AirbridgeScreenPasswords &&
       snapshot->screen != AirbridgeScreenPasswordTyping && app->passwords.count)
        airbridge_passwords_clear(&app->passwords);
    snapshot->mouse_enabled = app->mouse_enabled;
    snapshot->mouse_sent = app->mouse.sent;
    snapshot->mouse_failed = app->mouse.failed;
    snapshot->password_count = app->passwords.count;
    snapshot->password_selected = app->password_selected;
    size_t first = (app->password_selected / 3) * 3;
    for(size_t i = 0; i < 3 && first + i < app->passwords.count; i++)
        snprintf(
            snapshot->password_names[i],
            sizeof(snapshot->password_names[i]),
            "%s",
            app->passwords.entries[first + i].name);
    snprintf(snapshot->menu_status, sizeof(snapshot->menu_status), "%s", app->menu_status);
}
