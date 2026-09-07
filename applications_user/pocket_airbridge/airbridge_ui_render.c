#include "airbridge_ui_i.h"

#include <stdio.h>

#include "airbridge_usb.h"

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
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "USB Deploy");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    canvas_draw_str(canvas, 0, 31, "Place cursor in browser");
    canvas_draw_str(canvas, 0, 42, "console, then press OK");
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
