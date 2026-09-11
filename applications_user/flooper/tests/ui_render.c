#include "ui_fake.h"
#include "../flooper_ui.h"

unsigned ui_cases;
static void draw(FlooperUi* ui) {
    flooper_ui_draw(&ui_fake.canvas, ui);
    ui_capture();
}
void test_ui_render(void) {
    fake_reset(1000, 0);
    fake.app_mode = true;
    ui_fake_reset();
    FlooperUi ui = {.mutex = furi_mutex_alloc(FuriMutexTypeNormal)};
    /* Given a splash model; When drawn; Then exact brand text and Canvas animal. */
    draw(&ui);
    assert(ui_has_text("FLOOPER") && ui_has_text("the dolphin compás looper"));
    assert(ui_count_kind(UiLine) == 13 && ui_count_kind(UiDot) == 1);
    ui_cases++;
    /* Given selector names including wide UTF-8; When drawn; Then centered clipping. */
    ui.model.screen = FlooperSelector;
    snprintf(ui.model.selected_name, sizeof(ui.model.selected_name), "WWWWWWWWWWWWWWWWWWWWé");
    draw(&ui);
    assert(
        ui_has_text("SELECT LOOP") && ui_has_text("UP/DOWN   OK LOAD") &&
        ui_has_text("HOLD BACK EXIT"));
    bool clipped = false;
    for(unsigned i = 0; i < ui_fake.canvas.count; ++i) {
        const UiElement* e = &ui_fake.canvas.elements[i];
        if(e->kind == UiText && strstr(e->text, "...")) {
            clipped = true;
            assert(e->x >= 2 && e->text[strlen(e->text) - 4] != (char)0xc3);
        }
    }
    assert(clipped);
    ui_cases++;
    /* Given every legal count size and position; When drawn; Then index inversion,
     * all accent markers, numeric font and one/two rows independent of label values. */
    ui.model.screen = FlooperPlayback;
    for(uint8_t n = 1; n <= 12; ++n) {
        FlooperSnapshot s = {.state = FlooperStatePaused, .count_count = n, .accent_count = n};
        snprintf(s.display_name, sizeof(s.display_name), "DOCUMENT");
        snprintf(s.section_name, sizeof(s.section_name), "SECTION");
        for(uint8_t i = 0; i < n; ++i)
            s.count_labels[i] = s.accent_labels[i] = 99 - i;
        for(uint8_t position = 0; position < n; ++position) {
            s.count_index = position;
            s.count_label = s.count_labels[position];
            ui.model.snapshot = s;
            draw(&ui);
            unsigned labels = 0, accents = 0, inverted = 0, big = 0;
            for(unsigned i = 0; i < ui_fake.canvas.count; ++i) {
                const UiElement* e = &ui_fake.canvas.elements[i];
                if(e->kind == UiText && e->font == FontBigNumbers) {
                    assert(atoi(e->text) == s.count_label);
                    big++;
                }
                if(e->kind == UiText && e->y >= 52) {
                    assert(e->y == (n > 6 && labels < 6 ? 52 : 63));
                    assert(atoi(e->text) == s.count_labels[labels]);
                    assert((e->color == ColorWhite) == (labels == position));
                    inverted += e->color == ColorWhite;
                    labels++;
                }
                if(e->kind == UiLine && (e->y == 43 || e->y == 54)) accents++;
            }
            assert(labels == n && accents == n && inverted == 1 && big == 1);
        }
        ui_cases++;
    }
    /* Given new label 3; When ticking repeated snapshots through 80ms; Then pose
     * expires without retrigger, and leaving/reentering 3 triggers again. */
    for(unsigned doc = 0; doc < 2; ++doc) {
        ui.model = (FlooperUiModel){.screen = FlooperPlayback};
        FlooperSnapshot s = {
            .generation = 1, .state = FlooperStatePaused, .count_count = 4, .count_label = 3};
        snprintf(s.display_name, sizeof(s.display_name), "%s", doc ? "TANGOS" : "BULERIAS");
        flooper_ui_tick(&ui.model, &s, 100);
        assert(ui.model.contact);
        draw(&ui);
        assert(ui_count_kind(UiLine) == 14);
        flooper_ui_tick(&ui.model, &s, 179);
        assert(ui.model.contact);
        flooper_ui_tick(&ui.model, &s, 180);
        assert(!ui.model.contact);
        draw(&ui);
        assert(ui_count_kind(UiLine) == 13);
        flooper_ui_tick(&ui.model, &s, 200);
        assert(!ui.model.contact);
        s.count_label = 4;
        flooper_ui_tick(&ui.model, &s, 201);
        s.count_label = 3;
        flooper_ui_tick(&ui.model, &s, 202);
        assert(ui.model.contact);
        ui_cases++;
    }
    /* Given typed non-playable snapshots; When drawn; Then exit hints are qualified. */
    for(FlooperState state = FlooperStateLoading; state <= FlooperStateQuitting; ++state) {
        if(state == FlooperStatePaused || state == FlooperStatePlaying) continue;
        ui.model.snapshot = (FlooperSnapshot){.state = state, .error = FlooperPlayerStorage};
        draw(&ui);
        assert(ui_has_text("HOLD BACK EXIT") && !ui_has_text("BACK EXIT"));
        if(state == FlooperStatePatternError)
            assert(ui_has_text("PATTERN ERROR") && ui_has_text("STORAGE"));
        if(state == FlooperStateSpeakerBusy) assert(ui_has_text("SPEAKER BUSY"));
        if(state == FlooperStateLoading) assert(ui_has_text("LOADING"));
        ui_cases++;
    }
    furi_mutex_free(ui.mutex);
}
