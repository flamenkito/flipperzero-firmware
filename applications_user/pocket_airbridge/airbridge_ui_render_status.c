#include "airbridge_ui_i.h"

#include <inttypes.h>
#include <stdio.h>

static const uint8_t icon_usb_outline[] = {
    0x3E,
    0x55,
    0x41,
    0x3E,
    0x04,
    0x04,
    0x04,
    0x1C,
};
static const uint8_t icon_usb_filled[] = {
    0x3E,
    0x6B,
    0x7F,
    0x3E,
    0x04,
    0x04,
    0x04,
    0x1C,
};
static const uint8_t icon_bt_rune[] = {
    0x04,
    0x0D,
    0x16,
    0x0C,
    0x0C,
    0x16,
    0x0D,
    0x04,
};
static const uint8_t icon_arrow_r[] = {0x04, 0x08, 0x1F, 0x08, 0x04};
static const uint8_t icon_trash[] = {
    0x7E,
    0x81,
    0x55,
    0x55,
    0x55,
    0x55,
    0x81,
    0x7E,
};
static const uint8_t icon_alert[] = {
    0x10,
    0x00,
    0x38,
    0x00,
    0x28,
    0x00,
    0x6C,
    0x00,
    0x6C,
    0x00,
    0xFE,
    0x00,
    0xEE,
    0x00,
    0xFF,
    0x01,
};

static void draw_usb_glyph(Canvas* canvas, uint8_t x, uint8_t y, bool connected) {
    canvas_draw_xbm(canvas, x, y, 7, 8, connected ? icon_usb_filled : icon_usb_outline);
}

static void draw_bt_glyph(Canvas* canvas, uint8_t x, uint8_t y, bool connected) {
    canvas_draw_xbm(canvas, x, y, 5, 8, icon_bt_rune);
    if(connected) canvas_draw_box(canvas, x, y + 6, 5, 2);
}

static void format_direction_counter(char* line, size_t size, uint32_t count) {
    if(count <= UINT32_C(99999999)) {
        snprintf(line, size, "%" PRIu32, count);
    } else if(count < UINT32_C(1000000000)) {
        snprintf(line, size, "%" PRIu32 "M", count / UINT32_C(1000000));
    } else {
        snprintf(
            line,
            size,
            "%" PRIu32 ".%02" PRIu32 "G",
            count / UINT32_C(1000000000),
            (count % UINT32_C(1000000000)) / UINT32_C(10000000));
    }
}

static void format_fault_counter(char* line, size_t size, uint32_t count) {
    if(count > UINT32_C(999)) {
        snprintf(line, size, "999+");
    } else {
        snprintf(line, size, "%" PRIu32, count);
    }
}

void airbridge_ui_render_bridge(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);

    draw_usb_glyph(canvas, 0, 24, snapshot->metrics.usb_connected);
    canvas_draw_xbm(canvas, 9, 26, 5, 5, icon_arrow_r);
    draw_bt_glyph(canvas, 16, 24, snapshot->ble_connected);
    draw_bt_glyph(canvas, 44, 24, snapshot->ble_connected);
    canvas_draw_xbm(canvas, 51, 26, 5, 5, icon_arrow_r);
    draw_usb_glyph(canvas, 58, 24, snapshot->metrics.usb_connected);
    canvas_draw_xbm(canvas, 88, 24, 8, 8, icon_trash);
    canvas_draw_xbm(canvas, 108, 24, 9, 8, icon_alert);

    char line[16];
    format_direction_counter(line, sizeof(line), snapshot->metrics.chunks_usb_to_ble);
    canvas_draw_str_aligned(canvas, 0, 35, AlignLeft, AlignTop, line);
    format_direction_counter(line, sizeof(line), snapshot->metrics.chunks_ble_to_usb);
    canvas_draw_str_aligned(canvas, 44, 35, AlignLeft, AlignTop, line);
    format_fault_counter(line, sizeof(line), snapshot->metrics.dropped);
    canvas_draw_str_aligned(canvas, 88, 35, AlignLeft, AlignTop, line);
    format_fault_counter(line, sizeof(line), snapshot->metrics.tx_errors);
    canvas_draw_str_aligned(canvas, 108, 35, AlignLeft, AlignTop, line);
    if(snapshot->mouse_enabled) canvas_draw_str(canvas, 0, 55, "Mouse: ON");
}
