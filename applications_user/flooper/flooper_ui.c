#include "flooper_ui.h"
#include <stdio.h>
#include <string.h>

void flooper_ui_tick(FlooperUiModel* m, const FlooperSnapshot* s, uint32_t tick) {
    if(s && (uint32_t)(s->generation - m->minimum_generation) < UINT32_MAX / 2) {
        m->minimum_generation = s->generation;
        m->snapshot = *s;
        if(m->screen == FlooperPlayback && s->count_count) {
            if(s->count_label == 3 && m->last_label != 3) {
                m->pose_tick = tick;
                m->contact = true;
            }
            m->last_label = s->count_label;
        }
    }
    if((uint32_t)(tick - m->pose_tick) >= furi_ms_to_ticks(80)) m->contact = false;
}

typedef struct {
    int32_t center, baseline;
    uint16_t width;
} TextLine;
static void text_line(Canvas* c, const char* text, TextLine line) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s", text);
    size_t length = strlen(buffer);
    if(canvas_string_width(c, buffer) > line.width) {
        do {
            do {
                --length;
            } while(length && ((unsigned char)buffer[length] & 0xc0) == 0x80);
            snprintf(buffer + length, sizeof(buffer) - length, "...");
        } while(length && canvas_string_width(c, buffer) > line.width);
    }
    canvas_draw_str(c, line.center - canvas_string_width(c, buffer) / 2, line.baseline, buffer);
}
static void centered(Canvas* c, const char* text, int32_t y) {
    text_line(c, text, (TextLine){.center = 64, .baseline = y, .width = 124});
}

static void dolphin(Canvas* c, int32_t x, bool contact) {
    const int32_t y = 27;
    canvas_draw_line(c, x, y + 8, x + 6, y + 3);
    canvas_draw_line(c, x + 6, y + 3, x + 11, y + 3);
    canvas_draw_line(c, x + 11, y + 3, x + 13, y);
    canvas_draw_line(c, x + 13, y, x + 16, y + 3);
    canvas_draw_line(c, x + 16, y + 3, x + 23, y + 4);
    canvas_draw_line(c, x + 23, y + 4, x + 28, y + 7);
    canvas_draw_line(c, x + 28, y + 7, x + 21, y + 8);
    canvas_draw_line(c, x + 21, y + 8, x + 8, y + 10);
    canvas_draw_line(c, x + 8, y + 10, x, y + 8);
    canvas_draw_line(c, x, y + 8, x, y + 3);
    canvas_draw_line(c, x, y + 3, x + 4, y + 7);
    canvas_draw_dot(c, x + 22, y + 5);
    canvas_draw_line(c, x + 13, y + 8, x + (contact ? 19 : 10), y + 11);
    canvas_draw_line(c, x + 19, y + 8, x + (contact ? 19 : 23), y + 11);
    if(contact) canvas_draw_line(c, x + 17, y + 13, x + 21, y + 13);
}

static const char* state_name(FlooperState state) {
    switch(state) {
    case FlooperStateLoading:
        return "LOADING";
    case FlooperStatePaused:
        return "PAUSED";
    case FlooperStatePlaying:
        return "PLAYING";
    case FlooperStatePatternError:
        return "PATTERN ERROR";
    case FlooperStateSpeakerBusy:
        return "SPEAKER BUSY";
    case FlooperStateQuitting:
        return "QUITTING";
    }
    return "STATE ERROR";
}

static const char* error_name(FlooperPlayerError error) {
    switch(error) {
    case FlooperPlayerOk:
        return "";
    case FlooperPlayerMemory:
        return "MEMORY";
    case FlooperPlayerSelection:
        return "SELECTION";
    case FlooperPlayerStorage:
        return "STORAGE";
    case FlooperPlayerSize:
        return "SIZE";
    case FlooperPlayerParse:
        return "PARSE";
    case FlooperPlayerCompile:
        return "COMPILE";
    case FlooperPlayerClock:
        return "CLOCK";
    }
    return "ERROR";
}

static void labels(Canvas* c, const FlooperSnapshot* s) {
    canvas_set_font(c, FontSecondary);
    for(uint8_t i = 0; i < s->count_count; ++i) {
        unsigned row_count = s->count_count > 6 ? (i < 6 ? 6 : s->count_count - 6) :
                                                  s->count_count;
        int32_t x = 64 - (int32_t)row_count * 10 + (i % 6) * 20;
        int32_t y = s->count_count > 6 && i < 6 ? 52 : 63;
        char number[4];
        snprintf(number, sizeof(number), "%u", s->count_labels[i]);
        bool current = i == s->count_index;
        if(current) canvas_draw_box(c, x, y - 7, 18, 8);
        if(current) canvas_set_color(c, ColorWhite);
        canvas_draw_str(c, x + (18 - canvas_string_width(c, number)) / 2, y, number);
        canvas_set_color(c, ColorBlack);
        for(uint8_t a = 0; a < s->accent_count; ++a)
            if(s->accent_labels[a] == s->count_labels[i])
                canvas_draw_line(c, x + 6, y - 9, x + 11, y - 9);
    }
}

void flooper_ui_draw(Canvas* c, void* context) {
    FlooperUi* ui = context;
    if(furi_mutex_acquire(ui->mutex, FuriWaitForever) != FuriStatusOk) return;
    const FlooperUiModel m = ui->model;
    furi_mutex_release(ui->mutex);
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    canvas_set_font(c, FontPrimary);
    switch(m.screen) {
    case FlooperSplash:
        centered(c, "FLOOPER", 17);
        dolphin(c, 50, false);
        canvas_set_font(c, FontSecondary);
        centered(c, "the dolphin compás looper", 61);
        break;
    case FlooperSelector:
        centered(c, "FLOOPER", 10);
        canvas_set_font(c, FontSecondary);
        centered(c, "SELECT LOOP", 23);
        canvas_set_font(c, FontPrimary);
        centered(c, m.selected_name, 37);
        canvas_set_font(c, FontSecondary);
        centered(c, "UP/DOWN   OK LOAD", 49);
        centered(c, "HOLD BACK EXIT", 63);
        break;
    case FlooperPlayback: {
        const FlooperSnapshot* s = &m.snapshot;
        canvas_set_font(c, FontSecondary);
        centered(c, s->display_name, 8);
        switch(s->state) {
        case FlooperStatePaused:
        case FlooperStatePlaying: {
            char line[40];
            canvas_draw_str(c, 2, 19, state_name(s->state));
            snprintf(line, sizeof(line), "C%u %s", s->cycle_index + 1, s->section_name);
            text_line(c, line, (TextLine){.center = 87, .baseline = 19, .width = 78});
            char number[4];
            snprintf(number, sizeof(number), "%u", s->count_label);
            canvas_set_font(c, FontBigNumbers);
            canvas_draw_str(c, 48 - canvas_string_width(c, number) / 2, 40, number);
            dolphin(c, 94, m.contact);
            labels(c, s);
            break;
        }
        case FlooperStatePatternError:
            centered(c, state_name(s->state), 17);
            centered(c, error_name(s->error), 35);
            centered(c, "HOLD BACK EXIT", 63);
            break;
        case FlooperStateSpeakerBusy:
        case FlooperStateLoading:
        case FlooperStateQuitting:
            centered(c, state_name(s->state), 17);
            centered(c, "HOLD BACK EXIT", 63);
            break;
        }
        break;
    }
    }
}
