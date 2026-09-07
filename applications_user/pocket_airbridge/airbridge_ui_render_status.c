#include "airbridge_ui_i.h"

#include <stdio.h>

static const uint8_t icon_usb_outline[] = {
    0x3E, 0x55, 0x41, 0x3E, 0x04, 0x04, 0x04, 0x1C,
};
static const uint8_t icon_usb_filled[] = {
    0x3E, 0x6B, 0x7F, 0x3E, 0x04, 0x04, 0x04, 0x1C,
};
static const uint8_t icon_bt_rune[] = {
    0x04, 0x0D, 0x16, 0x0C, 0x0C, 0x16, 0x0D, 0x04,
};
static const uint8_t icon_arrow_r[] = {0x04, 0x08, 0x1F, 0x08, 0x04};
static const uint8_t icon_trash[] = {
    0x7E, 0x81, 0x55, 0x55, 0x55, 0x55, 0x81, 0x7E,
};
static const uint8_t icon_alert[] = {
    0x10, 0x00, 0x38, 0x00, 0x28, 0x00, 0x6C, 0x00,
    0x6C, 0x00, 0xFE, 0x00, 0xEE, 0x00, 0xFF, 0x01,
};

static void draw_usb_glyph(Canvas* canvas, uint8_t x, uint8_t y, bool connected) {
    canvas_draw_xbm(canvas, x, y, 7, 8, connected ? icon_usb_filled : icon_usb_outline);
}

static void draw_bt_glyph(Canvas* canvas, uint8_t x, uint8_t y, bool connected) {
    canvas_draw_xbm(canvas, x, y, 5, 8, icon_bt_rune);
    if(connected) canvas_draw_box(canvas, x, y + 6, 5, 2);
}

void airbridge_ui_render_bridge(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");
    canvas_set_font(canvas, FontSecondary);
    airbridge_ui_draw_identity(canvas, snapshot, 19);

    draw_usb_glyph(canvas, 0, 24, snapshot->metrics.usb_connected);
    canvas_draw_xbm(canvas, 9, 26, 5, 5, icon_arrow_r);
    draw_bt_glyph(canvas, 16, 24, snapshot->ble_connected);
    draw_bt_glyph(canvas, 32, 24, snapshot->ble_connected);
    canvas_draw_xbm(canvas, 39, 26, 5, 5, icon_arrow_r);
    draw_usb_glyph(canvas, 46, 24, snapshot->metrics.usb_connected);
    canvas_draw_xbm(canvas, 64, 24, 8, 8, icon_trash);
    canvas_draw_xbm(canvas, 96, 24, 9, 8, icon_alert);

    char line[16];
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.chunks_usb_to_ble);
    canvas_draw_str_aligned(canvas, 0, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.chunks_ble_to_usb);
    canvas_draw_str_aligned(canvas, 32, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.dropped);
    canvas_draw_str_aligned(canvas, 64, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.tx_errors);
    canvas_draw_str_aligned(canvas, 96, 35, AlignLeft, AlignTop, line);
}
