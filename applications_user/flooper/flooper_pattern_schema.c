#include "flooper_pattern_internal.h"
#include <string.h>

void fp_timebase(FlooperReader* r, bool draft) {
    FlooperPattern* d = r->out;
    uint16_t time = fp_get(r, 1, "timebase");
    if(!fp_type(r, time, JsonObject)) return;
    const char* forbidden[] = {
        "pulse_seconds", "pulse_bpm", "cycle_seconds", "cycle_us", "cycle_counts"};
    if(!draft) {
        for(unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
            if(fp_get(r, time, forbidden[i])) fp_fail(r, FlooperPatternDerivedTime);
        for(uint16_t i = time + 1; i < r->json->tokens[time].next; i = r->json->tokens[i + 1].next)
            if(!json_min_equal(r->json, i, "count_labels") &&
               !json_min_equal(r->json, i, "pulse_us") &&
               !json_min_equal(r->json, i, "subdivisions_per_count"))
                fp_fail(r, FlooperPatternType);
    }
    uint16_t labels = fp_get(r, time, "count_labels");
    d->count_count = fp_array(r, labels, FLOOPER_COUNTS_MAX);
    if(!d->count_count) fp_fail(r, FlooperPatternLimit);
    for(unsigned i = 0; i < d->count_count; ++i) {
        d->labels[i] = fp_uint(r, fp_at(r, labels, i), 1, 99);
        for(unsigned k = 0; k < i; ++k)
            if(d->labels[i] == d->labels[k]) fp_fail(r, FlooperPatternDuplicate);
    }
    d->pulse_us = fp_uint(r, fp_get(r, time, "pulse_us"), 100000, 2000000);
    uint16_t hint = fp_get(r, time, "subdivisions_per_count");
    if(hint) (void)fp_uint(r, hint, 1, UINT32_MAX);
    d->cycle_us = (uint64_t)d->pulse_us * d->count_count;
}

void fp_voices(FlooperReader* r) {
    FlooperPattern* d = r->out;
    uint16_t voices = fp_get(r, 1, "voices");
    d->voice_count = fp_array(r, voices, FLOOPER_ITEMS_MAX);
    for(unsigned i = 0; i < d->voice_count && r->error == FlooperPatternOk; ++i) {
        uint16_t item = fp_at(r, voices, i);
        FlooperVoice* v = &d->voices[i];
        fp_string(r, fp_get(r, item, "id"), v->id, sizeof(v->id));
        for(unsigned k = 0; k < i; ++k)
            if(strcmp(v->id, d->voices[k].id) == 0) fp_fail(r, FlooperPatternDuplicate);
        v->priority = fp_uint(r, fp_get(r, item, "priority"), 0, 255);
        uint16_t slices = fp_get(r, item, "slices");
        v->slice_count = fp_array(r, slices, FLOOPER_SLICES_MAX);
        if(!v->slice_count) fp_fail(r, FlooperPatternLimit);
        for(unsigned s = 0; s < v->slice_count; ++s) {
            uint16_t slice = fp_at(r, slices, s);
            FlooperSlice* out = &v->slices[s];
            out->offset_us = fp_uint(r, fp_get(r, slice, "offset_us"), 0, UINT32_MAX);
            out->duration_us = fp_uint(r, fp_get(r, slice, "duration_us"), 1, UINT32_MAX);
            out->frequency_hz = fp_real(r, fp_get(r, slice, "frequency_hz"), 100, 10000);
            out->gain = fp_real(r, fp_get(r, slice, "gain"), 0, 1);
            if((uint64_t)out->offset_us + out->duration_us > d->pulse_us)
                fp_fail(r, FlooperPatternContainment);
        }
    }
}

void fp_events(FlooperReader* r, uint16_t array, unsigned pattern, bool draft) {
    FlooperPattern* d = r->out;
    FlooperEventPattern* p = &d->patterns[pattern];
    p->first_event = d->event_count;
    p->event_count = fp_array(r, array, FLOOPER_PATTERN_EVENTS_MAX);
    if(d->event_count + p->event_count > FLOOPER_EVENTS_MAX) {
        fp_fail(r, FlooperPatternLimit);
        return;
    }
    for(unsigned i = 0; i < p->event_count && r->error == FlooperPatternOk; ++i) {
        uint16_t item = fp_at(r, array, i);
        FlooperEvent* e = &d->events[d->event_count++];
        e->count_index = fp_label(r, fp_get(r, item, "count"));
        e->offset_us = fp_uint(r, fp_get(r, item, "offset_us"), 0, d->pulse_us - 1);
        e->voice = fp_reference(r, fp_get(r, item, "voice"), 1);
        e->layer = draft ? 0 : fp_reference(r, fp_get(r, item, "layer"), 0);
        if(draft && fp_get(r, item, "layer") &&
           !json_min_equal(r->json, fp_get(r, item, "layer"), d->layers[0]))
            fp_fail(r, FlooperPatternReference);
        e->velocity = fp_real(r, fp_get(r, item, "velocity"), 0, 1);
        uint16_t intensity = fp_get(r, item, "source_intensity");
        e->has_source_intensity = intensity != 0;
        if(intensity) e->source_intensity = fp_real(r, intensity, 0, 1);
    }
}

void fp_sections(FlooperReader* r, bool draft) {
    FlooperPattern* d = r->out;
    uint16_t sections = fp_get(r, 1, "sections");
    d->section_count = fp_array(r, sections, FLOOPER_ITEMS_MAX);
    if(!d->section_count) fp_fail(r, FlooperPatternLimit);
    for(unsigned i = 0; i < d->section_count && r->error == FlooperPatternOk; ++i) {
        uint16_t item = fp_at(r, sections, i);
        FlooperSection* s = &d->sections[i];
        fp_string(r, fp_get(r, item, "name"), s->name, sizeof(s->name));
        s->pattern = draft ? 0 : fp_reference(r, fp_get(r, item, "pattern_id"), 2);
        s->cycles = fp_uint(r, fp_get(r, item, "cycles"), 1, 65535);
        uint16_t layers = fp_get(r, item, "layers");
        unsigned count = fp_array(r, layers, FLOOPER_ITEMS_MAX);
        for(unsigned k = 0; k < count; ++k) {
            uint8_t mask = 1u << fp_reference(r, fp_at(r, layers, k), 0);
            if(s->layer_mask & mask) fp_fail(r, FlooperPatternDuplicate);
            s->layer_mask |= mask;
        }
    }
    uint16_t loop = fp_get(r, 1, "loop");
    uint32_t start = fp_uint(r, fp_get(r, loop, "start_section"), 0, UINT32_MAX);
    if(start >= d->section_count)
        fp_fail(r, FlooperPatternReference);
    else
        d->start_section = start;
    d->repeat = fp_bool(r, fp_get(r, loop, "repeat"));
}

void fp_finish(FlooperReader* r) {
    FlooperPattern* d = r->out;
    if(r->error != FlooperPatternOk) return;
    for(unsigned p = 0; p < d->pattern_count; ++p) {
        unsigned candidates[FLOOPER_COUNTS_MAX] = {0};
        FlooperEventPattern* pattern = &d->patterns[p];
        for(unsigned i = pattern->first_event; i < pattern->first_event + pattern->event_count;
            ++i) {
            FlooperEvent* e = &d->events[i];
            FlooperVoice* v = &d->voices[e->voice];
            candidates[e->count_index] += v->slice_count;
            if(candidates[e->count_index] > FLOOPER_CANDIDATES_MAX)
                fp_fail(r, FlooperPatternLimit);
            for(unsigned s = 0; s < v->slice_count; ++s)
                if((uint64_t)e->offset_us + v->slices[s].offset_us + v->slices[s].duration_us >
                   d->pulse_us)
                    fp_fail(r, FlooperPatternContainment);
        }
    }
}

void fp_canonical(FlooperReader* r) {
    FlooperPattern* d = r->out;
    fp_string(r, fp_get(r, 1, "name"), d->name, sizeof(d->name));
    fp_string(r, fp_get(r, 1, "display_name"), d->display_name, sizeof(d->display_name));
    fp_timebase(r, false);
    uint16_t layers = fp_get(r, 1, "layers");
    d->layer_count = fp_array(r, layers, FLOOPER_ITEMS_MAX);
    for(unsigned i = 0; i < d->layer_count; ++i) {
        fp_string(r, fp_at(r, layers, i), d->layers[i], sizeof(d->layers[i]));
        for(unsigned k = 0; k < i; ++k)
            if(strcmp(d->layers[i], d->layers[k]) == 0) fp_fail(r, FlooperPatternDuplicate);
    }
    fp_voices(r);
    uint16_t patterns = fp_get(r, 1, "patterns");
    d->pattern_count = fp_array(r, patterns, FLOOPER_ITEMS_MAX);
    for(unsigned i = 0; i < d->pattern_count && r->error == FlooperPatternOk; ++i) {
        uint16_t item = fp_at(r, patterns, i);
        fp_string(r, fp_get(r, item, "id"), d->patterns[i].id, sizeof(d->patterns[i].id));
        for(unsigned k = 0; k < i; ++k)
            if(strcmp(d->patterns[i].id, d->patterns[k].id) == 0)
                fp_fail(r, FlooperPatternDuplicate);
        fp_events(r, fp_get(r, item, "events"), i, false);
    }
    fp_sections(r, false);
}
