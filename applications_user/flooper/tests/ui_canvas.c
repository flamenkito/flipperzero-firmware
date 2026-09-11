#include "ui_fake.h"

static UiElement* element(Canvas* c, UiElementKind kind) {
    assert(!fake_worker && fake_locks == 0);
    assert(c->count < 128);
    UiElement* e = &c->elements[c->count++];
    *e = (UiElement){.kind = kind, .font = c->font, .color = c->color};
    return e;
}
void canvas_clear(Canvas* c) {
    assert(!fake_worker && fake_locks == 0);
    c->count = 0;
    if(ui_fake.draw_hook) ui_fake.draw_hook();
}
void canvas_set_color(Canvas* c, Color color) {
    assert(fake_locks == 0);
    c->color = color;
}
void canvas_set_font(Canvas* c, Font font) {
    assert(fake_locks == 0);
    c->font = font;
}
uint16_t canvas_string_width(Canvas* c, const char* text) {
    assert(fake_locks == 0);
    unsigned width = 0;
    for(const unsigned char* p = (const unsigned char*)text; *p; ++p)
        if((*p & 0xc0) != 0x80) {
            unsigned glyph = c->font == FontBigNumbers ? 12 : c->font == FontPrimary ? 6 : 5;
            if(c->font == FontSecondary && strchr(" ilt", *p)) glyph = 3;
            width += glyph;
        }
    return width;
}
void canvas_draw_str(Canvas* c, int32_t x, int32_t y, const char* text) {
    assert(x >= 0 && x + canvas_string_width(c, text) <= 128 && y >= 0 && y <= 63);
    UiElement* e = element(c, UiText);
    e->x = x;
    e->y = y;
    assert(snprintf(e->text, sizeof(e->text), "%s", text) < (int)sizeof(e->text));
}
void canvas_draw_line(Canvas* c, int32_t x, int32_t y, int32_t x2, int32_t y2) {
    assert(x >= 0 && x < 128 && y >= 0 && y < 64);
    assert(x2 >= 0 && x2 < 128 && y2 >= 0 && y2 < 64);
    UiElement* e = element(c, UiLine);
    e->x = x;
    e->y = y;
    e->x2 = x2;
    e->y2 = y2;
}
void canvas_draw_dot(Canvas* c, int32_t x, int32_t y) {
    UiElement* e = element(c, UiDot);
    e->x = x;
    e->y = y;
    assert(x >= 0 && x < 128 && y >= 0 && y < 64);
}
void canvas_draw_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    UiElement* e = element(c, UiBox);
    e->x = x;
    e->y = y;
    e->x2 = x + (int32_t)w;
    e->y2 = y + (int32_t)h;
    assert(x >= 0 && y >= 0 && e->x2 <= 128 && e->y2 <= 64);
}
bool ui_has_text(const char* text) {
    for(unsigned i = 0; i < ui_fake.canvas.count; ++i)
        if(ui_fake.canvas.elements[i].kind == UiText &&
           strcmp(ui_fake.canvas.elements[i].text, text) == 0)
            return true;
    return false;
}
unsigned ui_count_kind(UiElementKind kind) {
    unsigned count = 0;
    for(unsigned i = 0; i < ui_fake.canvas.count; ++i)
        count += ui_fake.canvas.elements[i].kind == kind;
    return count;
}
void ui_capture(void) {
    const char* path = getenv("FLOOPER_CANVAS_TRACE");
    if(!path) return;
    FILE* output = fopen(path, "a");
    assert(output);
    fprintf(
        output,
        "FRAME tick=%lu primitives=%u\n",
        (unsigned long)furi_get_tick(),
        ui_fake.canvas.count);
    for(unsigned i = 0; i < ui_fake.canvas.count; ++i) {
        const UiElement* e = &ui_fake.canvas.elements[i];
        fprintf(
            output,
            "kind=%d xy=%ld,%ld end=%ld,%ld font=%d color=%d text=%s\n",
            e->kind,
            (long)e->x,
            (long)e->y,
            (long)e->x2,
            (long)e->y2,
            e->font,
            e->color,
            e->text);
    }
    assert(fclose(output) == 0);
}
