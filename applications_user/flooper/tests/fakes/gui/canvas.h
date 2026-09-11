#pragma once
#include <stdint.h>
#include <stddef.h>
typedef enum {
    ColorWhite,
    ColorBlack,
    ColorXOR
} Color;
typedef enum {
    FontPrimary,
    FontSecondary,
    FontKeyboard,
    FontBigNumbers,
    FontTotalNumber
} Font;
typedef struct Canvas Canvas;
void canvas_clear(Canvas* canvas);
void canvas_set_color(Canvas* canvas, Color color);
void canvas_set_font(Canvas* canvas, Font font);
uint16_t canvas_string_width(Canvas* canvas, const char* text);
void canvas_draw_str(Canvas* canvas, int32_t x, int32_t y, const char* text);
void canvas_draw_line(Canvas* canvas, int32_t x, int32_t y, int32_t x2, int32_t y2);
void canvas_draw_dot(Canvas* canvas, int32_t x, int32_t y);
void canvas_draw_box(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height);
