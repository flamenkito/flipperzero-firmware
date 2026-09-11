#pragma once
#include "../flooper_schedule.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host assertions must run even with the firmware compilation database's NDEBUG. */
#undef assert
#define assert(condition)                                                   \
    do {                                                                    \
        if(!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                        \
        }                                                                   \
    } while(0)

static inline FlooperPattern fixture(void) {
    FlooperPattern m = {0};
    m.pulse_us = m.cycle_us = 100;
    m.count_count = m.layer_count = m.voice_count = 1;
    m.pattern_count = m.section_count = 1;
    m.labels[0] = 7;
    m.master_volume = 1;
    m.voices[0].slice_count = 1;
    m.voices[0].slices[0] = (FlooperSlice){0, 80, 440, 1};
    m.sections[0] = (FlooperSection){.cycles = 2, .layer_mask = 1};
    m.event_count = m.patterns[0].event_count = 1;
    m.events[0].velocity = 1;
    return m;
}

static inline void compile_ok(const FlooperPattern* m, FlooperSchedule* s) {
    FlooperPattern before = *m;
    assert(flooper_schedule_compile(m, s) == FlooperScheduleOk);
    assert(memcmp(&before, m, sizeof(before)) == 0);
    for(unsigned r = 0; r < s->range_count; ++r) {
        const FlooperCountRange* range = &s->ranges[r];
        uint32_t end = 0;
        for(unsigned i = 0; i < range->interval_count; ++i) {
            const FlooperInterval* span = &s->intervals[range->first_interval + i];
            assert(span->start_us >= end && span->end_us > span->start_us);
            assert(span->end_us <= s->pulse_us && span->volume > 0 && span->volume <= 1);
            end = span->end_us;
        }
    }
}

static inline void compile_error(const FlooperPattern* m, FlooperScheduleError error) {
    static FlooperSchedule s;
    static const FlooperSchedule zero;
    memset(&s, 0xa5, sizeof(s));
    assert(flooper_schedule_compile(m, &s) == error);
    assert(memcmp(&s, &zero, sizeof(s)) == 0);
}

unsigned schedule_bounds(void);
unsigned schedule_music(const char* root);
