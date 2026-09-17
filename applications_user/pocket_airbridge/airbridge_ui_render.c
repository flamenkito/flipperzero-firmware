#include "airbridge_ui_i.h"

#include <stdio.h>
#include <string.h>

#include "airbridge_usb.h"

static void airbridge_ui_menu_row(Canvas* canvas, uint8_t y, const char* name, bool selected) {
    char text[32];
    snprintf(text, sizeof(text), "%s", name);
    while(canvas_string_width(canvas, text) > 120 && strlen(text))
        text[strlen(text) - 1] = '\0';
    if(selected) {
        canvas_draw_box(canvas, 0, y - 9, 128, 12);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str(canvas, 3, y, text);
    canvas_set_color(canvas, ColorBlack);
}

static void airbridge_ui_render_menu(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    bool settings = snapshot->screen == AirbridgeScreenSettings;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, settings ? "Settings" : "Passwords");
    canvas_set_font(canvas, FontSecondary);
    if(settings) {
        airbridge_ui_menu_row(
            canvas, 26, snapshot->mouse_enabled ? "Mouse mover: ON" : "Mouse mover: OFF", true);
        char mouse_status[32];
        snprintf(
            mouse_status,
            sizeof(mouse_status),
            "Moves: %lu  Err: %lu",
            (unsigned long)snapshot->mouse_sent,
            (unsigned long)snapshot->mouse_failed);
        canvas_draw_str(canvas, 0, 42, mouse_status);
        canvas_draw_str(canvas, 0, 53, snapshot->menu_status);
        canvas_draw_str(canvas, 0, 63, "OK: toggle   BACK: Bridge");
    } else {
        if(snapshot->password_count) {
            for(uint8_t i = 0; i < 3; i++) {
                if(!snapshot->password_names[i][0]) break;
                airbridge_ui_menu_row(
                    canvas,
                    24 + i * 12,
                    snapshot->password_names[i],
                    i == snapshot->password_selected % 3);
            }
            char count[12];
            snprintf(
                count,
                sizeof(count),
                "%u/%u",
                snapshot->password_selected + 1,
                snapshot->password_count);
            canvas_draw_str_aligned(canvas, 127, 10, AlignRight, AlignBottom, count);
        }
        if(snapshot->menu_status[0])
            canvas_draw_str(canvas, 0, 59, snapshot->menu_status);
        else
            canvas_draw_str(canvas, 0, 63, "OK: type USB   BACK: Bridge");
    }
}

void airbridge_ui_draw_identity(Canvas* canvas, const AirbridgeUiSnapshot* snapshot, uint8_t y) {
    if(snapshot->identity_warning) {
        canvas_draw_str(canvas, 0, y, "WARN: BLE ID DEFAULT");
        return;
    }
    const char* identity = airbridge_usb_profile_identity(snapshot->usb_profile_index);
    char line[32];
    snprintf(line, sizeof(line), "HID: %s", identity != NULL ? identity : "--");
    canvas_draw_str(canvas, 0, y, line);
}

void airbridge_ui_draw_progress(Canvas* canvas, uint8_t y, uint64_t position, uint64_t total) {
    canvas_draw_frame(canvas, 4, y, 120, 8);
    if(total == 0) return;
    uint32_t fill_width = (uint32_t)(118ULL * position / total);
    if(fill_width > 118) fill_width = 118;
    canvas_draw_box(canvas, 5, y + 1, fill_width, 6);
}

void airbridge_ui_render_deploy_prompt(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    const bool use_ble = snapshot->deploy_transport == AirbridgeTypingTransportBle;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, use_ble ? "BLE Deploy" : "USB Deploy");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    if(use_ble) {
        canvas_draw_str(canvas, 0, 31, "Pair in BT settings:");
        canvas_draw_str(canvas, 0, 42, snapshot->ble_name);
        canvas_draw_str(canvas, 0, 54, "then press OK");
    } else {
        canvas_draw_str(canvas, 0, 31, "Place cursor in browser");
        canvas_draw_str(canvas, 0, 42, "console, then press OK");
    }
}

void airbridge_ui_render_callback(Canvas* canvas, void* context) {
    AirbridgeUiSnapshot snapshot;
    airbridge_ui_snapshot_copy(context, &snapshot);
    canvas_clear(canvas);
    if(snapshot.closing || snapshot.operation.stalled) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(
            canvas, 0, 16, snapshot.operation.stalled ? "Operation stalled" : "Closing...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(
            canvas,
            0,
            34,
            snapshot.operation.stalled ? "No progress detected" : "Restoring connections");
        if(snapshot.operation.stalled) {
            canvas_draw_str(canvas, 0, 50, "Restart device manually");
        }
        if(snapshot.closing) airbridge_ui_closing_rendered(context);
        return;
    }
    switch(snapshot.screen) {
    case AirbridgeScreenBridge:
        airbridge_ui_render_bridge(canvas, &snapshot);
        break;
    case AirbridgeScreenSettings:
    case AirbridgeScreenPasswords:
        airbridge_ui_render_menu(canvas, &snapshot);
        break;
    case AirbridgeScreenPasswordTyping:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 0, 12, "TYPING via USB...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 0, 32, "Password");
        canvas_draw_str(canvas, 0, 63, "BACK: abort");
        airbridge_ui_password_rendered(context, snapshot.typing_generation);
        break;
    case AirbridgeScreenDeployPrompt:
        airbridge_ui_render_deploy_prompt(canvas, &snapshot);
        break;
    case AirbridgeScreenTyping:
        airbridge_ui_render_typing(canvas, &snapshot);
        break;
    case AirbridgeScreenWaiting:
        airbridge_ui_render_waiting(canvas, &snapshot);
        break;
    case AirbridgeScreenStreaming:
        airbridge_ui_render_streaming(canvas, &snapshot);
        break;
    case AirbridgeScreenDone:
        airbridge_ui_render_message(
            canvas, &snapshot, "Done", "Transfer complete", "Ready for another deploy");
        break;
    case AirbridgeScreenError:
        airbridge_ui_render_message(
            canvas, &snapshot, snapshot.error.title, snapshot.error.detail, snapshot.error.action);
        break;
    case AirbridgeScreenFatal:
        airbridge_ui_render_fatal(canvas, &snapshot);
        break;
    }
}
