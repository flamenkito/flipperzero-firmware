#pragma once
#include "player_fake.h"
#include "fakes/gui/gui.h"
typedef enum {
    UiText,
    UiLine,
    UiDot,
    UiBox
} UiElementKind;
typedef struct {
    UiElementKind kind;
    int32_t x, y, x2, y2;
    Font font;
    Color color;
    char text[64];
} UiElement;
struct Canvas {
    Font font;
    Color color;
    UiElement elements[128];
    unsigned count;
};
typedef struct {
    ViewPort* viewport;
    FuriTimer* timer;
    Canvas canvas;
    void* app_context;
    unsigned added, removed, disabled, freed, timer_stopped, timer_freed, updates;
    void (*draw_hook)(void);
    void (*remove_hook)(void);
} UiFake;
extern UiFake ui_fake;
void ui_fake_reset(void);
void ui_fake_draw(void);
void ui_fake_input(InputType type, InputKey key);
void ui_fake_tick(void);
bool ui_has_text(const char* text);
unsigned ui_count_kind(UiElementKind kind);
void ui_capture(void);
void test_ui_render(void);
void test_ui_app(const char* root);
void test_adapters_app(const char* root);
void test_ui_inflight(const char* root);
extern unsigned ui_cases;
