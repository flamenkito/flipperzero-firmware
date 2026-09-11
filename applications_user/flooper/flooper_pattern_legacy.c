#include "flooper_pattern_legacy.h"
#include <string.h>

void flooper_pattern_legacy_draft(FlooperReader* r) {
    FlooperPattern* d = r->out;
    fp_string(r, fp_get(r, 1, "name"), d->name, sizeof(d->name));
    memcpy(d->display_name, "TANGOS", sizeof("TANGOS"));
    fp_timebase(r, true);
    uint16_t time = fp_get(r, 1, "timebase");
    uint16_t cycle = fp_get(r, time, "cycle_us");
    if(cycle) {
        d->reported_cycle_us = fp_uint(r, cycle, 0, UINT32_MAX);
        d->warning_cycle_mismatch = d->reported_cycle_us != d->cycle_us;
    }
    uint16_t sections = fp_get(r, 1, "sections");
    unsigned n = fp_array(r, sections, FLOOPER_ITEMS_MAX);
    bool found = false;
    for(unsigned i = 0; i < n; ++i) {
        uint16_t layers = fp_get(r, fp_at(r, sections, i), "layers");
        unsigned count = fp_array(r, layers, FLOOPER_ITEMS_MAX);
        for(unsigned k = 0; k < count; ++k) {
            if(!json_min_equal(r->json, fp_at(r, layers, k), "guitar_proxy"))
                fp_fail(r, FlooperPatternUnsupported);
            found = true;
        }
    }
    if(!found) fp_fail(r, FlooperPatternUnsupported);
    d->layer_count = 1;
    memcpy(d->layers[0], "guitar_proxy", sizeof("guitar_proxy"));
    fp_voices(r);
    d->pattern_count = 1;
    memcpy(d->patterns[0].id, "compas", sizeof("compas"));
    fp_events(r, fp_get(r, 1, "events"), 0, true);
    fp_sections(r, true);
}

static void legacy_event(FlooperReader* r, FlooperEvent event) {
    if(r->out->event_count == FLOOPER_EVENTS_MAX) {
        fp_fail(r, FlooperPatternLimit);
        return;
    }
    r->out->events[r->out->event_count++] = event;
}

static void legacy_voices(FlooperReader* r) {
    FlooperPattern* d = r->out;
    uint16_t model = fp_get(r, 1, "speaker_model");
    (void)fp_uint(r, fp_get(r, model, "polyphony"), 1, 1);
    const char* ids[] = {"palmas", "low_hit", "tick"};
    const float gains[2][3] = {{1, 0.85f, 0.7f}, {0.85f, 1, 0.85f}};
    d->voice_count = 3;
    for(unsigned i = 0; i < 3; ++i) {
        FlooperVoice* v = &d->voices[i];
        strcpy(v->id, ids[i]);
        v->priority = 3 - i;
        if(i == 2) {
            v->slice_count = 2;
            v->slices[0] = (FlooperSlice){0, 9000, 1450, 1};
            v->slices[1] = (FlooperSlice){10000, 6000, 2050, 0.7f};
            continue;
        }
        uint16_t slices = fp_get(r, model, ids[i]);
        v->slice_count = fp_array(r, slices, FLOOPER_SLICES_MAX);
        if(v->slice_count != 3) fp_fail(r, FlooperPatternUnsupported);
        for(unsigned s = 0; s < v->slice_count && r->error == FlooperPatternOk; ++s) {
            uint16_t slice = fp_at(r, slices, s);
            v->slices[s].offset_us = fp_uint(r, fp_get(r, slice, "offset_ms"), 0, 2000) * 1000;
            v->slices[s].duration_us = fp_uint(r, fp_get(r, slice, "duration_ms"), 1, 2000) * 1000;
            v->slices[s].frequency_hz = fp_real(r, fp_get(r, slice, "frequency_hz"), 100, 10000);
            v->slices[s].gain = gains[i][s];
        }
    }
}

void flooper_pattern_legacy_v1(FlooperReader* r) {
    FlooperPattern* d = r->out;
    fp_string(r, fp_get(r, 1, "name"), d->name, sizeof(d->name));
    strcpy(d->display_name, "BULERIAS");
    uint16_t time = fp_get(r, 1, "timebase");
    (void)fp_uint(r, fp_get(r, time, "cycle_counts"), 6, 6);
    (void)fp_uint(r, fp_get(r, time, "subdivisions_per_count"), 2, 2);
    (void)fp_real(r, fp_get(r, time, "pulse_seconds"), 0.314667, 0.314667);
    d->pulse_us = 314667;
    d->count_count = 6;
    const uint8_t labels[] = {6, 1, 2, 3, 4, 5};
    memcpy(d->labels, labels, sizeof(labels));
    d->cycle_us = (uint64_t)d->pulse_us * d->count_count;
    d->layer_count = 2;
    strcpy(d->layers[0], "palmas");
    strcpy(d->layers[1], "perc");
    legacy_voices(r);
    uint16_t claps = fp_get(r, 1, "palmas_velocity_by_count");
    uint16_t lows = fp_get(r, 1, "perc_low_velocity_by_count");
    if(fp_array(r, claps, 6) != 6 || fp_array(r, lows, 6) != 6) fp_fail(r, FlooperPatternLimit);
    float clap[6] = {0}, low[6] = {0};
    for(unsigned i = 0; i < 6; ++i) {
        clap[i] = fp_real(r, fp_at(r, claps, i), 0, 1);
        low[i] = fp_real(r, fp_at(r, lows, i), 0, 1);
    }
    d->pattern_count = 3;
    strcpy(d->patterns[0].id, "silence");
    strcpy(d->patterns[1].id, "palmas");
    strcpy(d->patterns[2].id, "palmas_perc");
    for(unsigned i = 0; i < 6; ++i)
        legacy_event(r, (FlooperEvent){.count_index = i, .velocity = clap[i]});
    d->patterns[1].event_count = 6;
    d->patterns[2].first_event = 6;
    uint16_t ticks = fp_get(r, 1, "offbeat_ticks");
    unsigned tick_count = fp_array(r, ticks, FLOOPER_PATTERN_EVENTS_MAX - 12);
    for(unsigned i = 0; i < tick_count; ++i) {
        uint16_t tick = fp_at(r, ticks, i);
        (void)fp_label(r, fp_get(r, tick, "count"));
        uint16_t subdivision = fp_get(r, tick, "subdivision");
        if(fp_type(r, subdivision, JsonString) && !json_min_equal(r->json, subdivision, "+"))
            fp_fail(r, FlooperPatternUnsupported);
        (void)fp_real(r, fp_get(r, tick, "velocity"), 0, 1);
    }
    for(unsigned i = 0; i < 6 && r->error == FlooperPatternOk; ++i) {
        if(low[i] > 0)
            legacy_event(
                r, (FlooperEvent){.count_index = i, .voice = 1, .layer = 1, .velocity = low[i]});
        legacy_event(
            r,
            (FlooperEvent){
                .count_index = i, .offset_us = low[i] > 0 ? 42000 : 0, .velocity = clap[i]});
        for(unsigned k = 0; k < tick_count; ++k) {
            uint16_t tick = fp_at(r, ticks, k);
            if(fp_label(r, fp_get(r, tick, "count")) == i)
                legacy_event(
                    r,
                    (FlooperEvent){
                        .count_index = i,
                        .voice = 2,
                        .layer = 1,
                        .offset_us = 157334,
                        .velocity = fp_real(r, fp_get(r, tick, "velocity"), 0, 1)});
        }
    }
    d->patterns[2].event_count = d->event_count - 6;
    uint16_t sections = fp_get(r, 1, "sections");
    d->section_count = fp_array(r, sections, FLOOPER_ITEMS_MAX);
    if(d->section_count != 3) fp_fail(r, FlooperPatternUnsupported);
    const char* displays[] = {"INTRO", "PALMAS", "PALMAS+PERC"};
    const unsigned cycles[] = {4, 4, 12};
    for(unsigned i = 0; i < 3 && r->error == FlooperPatternOk; ++i) {
        uint16_t item = fp_at(r, sections, i);
        char source_name[FLOOPER_ID_BYTES];
        fp_string(r, fp_get(r, item, "name"), source_name, sizeof(source_name));
        FlooperSection* s = &d->sections[i];
        strcpy(s->name, displays[i]);
        s->pattern = i;
        s->cycles = fp_uint(r, fp_get(r, item, "cycles"), cycles[i], cycles[i]);
        uint16_t layers = fp_get(r, item, "layers");
        unsigned count = fp_array(r, layers, FLOOPER_ITEMS_MAX);
        for(unsigned k = 0; k < count; ++k) {
            uint8_t mask = 1u << fp_reference(r, fp_at(r, layers, k), 0);
            if(s->layer_mask & mask) fp_fail(r, FlooperPatternDuplicate);
            s->layer_mask |= mask;
        }
        if(s->layer_mask != (i == 0 ? 0 : i == 1 ? 1 : 3)) fp_fail(r, FlooperPatternUnsupported);
    }
    d->repeat = true;
    d->master_volume = 0.65f;
    d->accent_mask = 1u << 3;
}
