#include "airbridge_ui_i.h"

#include <stdio.h>

void airbridge_ui_render_typing(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        snapshot->deploy_transport == AirbridgeTypingTransportBle ? "TYPING via BLE" :
                                                                    "TYPING via USB");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    airbridge_ui_draw_progress(canvas, 28, snapshot->typing_position, snapshot->typing_total);

    char line[32];
    snprintf(
        line,
        sizeof(line),
        "%lu/%lu chars",
        (unsigned long)snapshot->typing_position,
        (unsigned long)snapshot->typing_total);
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, line);
    const uint32_t percent =
        snapshot->typing_total > 0 ?
            (uint32_t)(100ULL * snapshot->typing_position / snapshot->typing_total) :
            0;
    snprintf(line, sizeof(line), "%lu%%", percent);
    canvas_draw_str_aligned(canvas, 124, 45, AlignRight, AlignTop, line);
    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

void airbridge_ui_render_streaming(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        snapshot->deploy_transport == AirbridgeTypingTransportBle ? "Serving app via BLE" :
                                                                    "Serving app via USB");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    airbridge_ui_draw_progress(canvas, 28, snapshot->stream_sent, snapshot->stream_total);

    char line[32];
    const uint64_t sent_tenths = snapshot->stream_sent * 10ULL / 1024ULL;
    const uint64_t total_tenths = snapshot->stream_total * 10ULL / 1024ULL;
    snprintf(
        line,
        sizeof(line),
        "%lu.%lu/%lu.%lu KB",
        (unsigned long)(sent_tenths / 10),
        (unsigned long)(sent_tenths % 10),
        (unsigned long)(total_tenths / 10),
        (unsigned long)(total_tenths % 10));
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, line);
    const uint32_t percent =
        snapshot->stream_total > 0 ?
            (uint32_t)(100ULL * snapshot->stream_sent / snapshot->stream_total) :
            0;
    snprintf(line, sizeof(line), "%lu%%", percent);
    canvas_draw_str_aligned(canvas, 124, 45, AlignRight, AlignTop, line);
    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

void airbridge_ui_render_waiting(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Waiting for browser...");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    canvas_draw_frame(canvas, 4, 28, 120, 8);
    const uint32_t phase = (furi_get_tick() / 50) % 64;
    const uint32_t offset = phase < 32 ? phase : 63 - phase;
    canvas_draw_box(canvas, 5 + offset * (118 - 12) / 31, 29, 12, 6);
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, "Click Connect in the browser");
    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

void airbridge_ui_render_message(
    Canvas* canvas,
    const AirbridgeUiSnapshot* snapshot,
    const char* title,
    const char* detail,
    const char* action) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, title);
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    canvas_draw_str(canvas, 0, 35, detail);
    canvas_draw_str(canvas, 0, 47, action);
    canvas_draw_str(canvas, 0, 63, "BACK: Bridge");
}

void airbridge_ui_render_fatal(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Fatal startup error");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);
    canvas_draw_str(canvas, 0, 36, snapshot->error.detail);
    canvas_draw_str(canvas, 0, 63, "Long BACK: exit");
}
