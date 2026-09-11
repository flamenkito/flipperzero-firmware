#include "schedule_test.h"

unsigned schedule_bounds(void) {
    unsigned cases = 0;
    /* Given malformed normalized structures; when compiled; then typed failure
     * clears even dirty output before any caller can publish it. */
#define REJECT(change, error)                      \
    do {                                           \
        FlooperPattern m = fixture();              \
        change;                                    \
        compile_error(&m, FlooperSchedule##error); \
        ++cases;                                   \
    } while(0)
    REJECT(m.count_count = 13, Invalid);
    REJECT(m.count_count = 0, Invalid);
    REJECT(m.layer_count = 9, Invalid);
    REJECT(m.voice_count = 9, Invalid);
    REJECT(m.pattern_count = 9, Invalid);
    REJECT(m.section_count = 9, Invalid);
    REJECT(m.event_count = 129, Invalid);
    REJECT(m.voices[0].slice_count = 9, Invalid);
    REJECT(m.voices[0].slice_count = 0, Invalid);
    REJECT(m.pulse_us = 0, Invalid);
    REJECT(m.cycle_us = UINT64_MAX, Invalid);
    REJECT(m.master_volume = NAN, Invalid);
    REJECT(m.events[0].velocity = INFINITY, Invalid);
    REJECT(m.voices[0].slices[0].gain = NAN, Invalid);
    REJECT(m.voices[0].slices[0].frequency_hz = INFINITY, Invalid);
    REJECT(m.voices[0].slices[0].duration_us = 0, Invalid);
    REJECT(m.labels[0] = 0, Invalid);
    REJECT(m.labels[0] = 100, Invalid);
    REJECT((m.count_count = 2, m.cycle_us = 200, m.labels[1] = 7), Invalid);
    REJECT(m.sections[0].cycles = 0, Invalid);
    REJECT(m.start_section = 1, Reference);
    REJECT(m.sections[0].pattern = 1, Reference);
    REJECT(m.sections[0].layer_mask = 2, Reference);
    REJECT(m.events[0].count_index = 1, Reference);
    REJECT(m.events[0].voice = 1, Reference);
    REJECT(m.events[0].layer = 1, Reference);
    REJECT(m.patterns[0].first_event = UINT16_MAX, Reference);
    REJECT(m.patterns[0].event_count = 65, Invalid);
    REJECT(m.events[0].offset_us = 21, Containment);
    REJECT(m.events[0].offset_us = UINT32_MAX, Overflow);
    REJECT(m.voices[0].slices[0].offset_us = UINT32_MAX, Overflow);
    REJECT((m.sections[0].layer_mask = 0, m.events[0].offset_us = 21), Containment);
#undef REJECT
    /* Given 64 expanded slices; when compiled; then exactly the cap is legal. */
    FlooperPattern m = fixture();
    static FlooperSchedule s;
    m.voices[0].slice_count = 8;
    for(unsigned j = 0; j < 8; ++j)
        m.voices[0].slices[j] = (FlooperSlice){j * 10, 1, 440, 1};
    m.event_count = m.patterns[0].event_count = 8;
    for(unsigned j = 0; j < 8; ++j)
        m.events[j] = m.events[0];
    compile_ok(&m, &s);
    assert(s.interval_count == 8);
    ++cases;
    m.event_count = m.patterns[0].event_count = 9;
    m.events[8] = m.events[0];
    compile_error(&m, FlooperScheduleCandidateLimit);
    ++cases;
    /* Given 128 events, 8 patterns/sections/layers/voices and 12 labels;
     * when compiled; then 1024 unique stored spans fit, the next does not. */
    m = fixture();
    m.count_count = 12;
    m.cycle_us = 1200;
    m.layer_count = m.voice_count = m.pattern_count = m.section_count = 8;
    m.event_count = 128;
    for(unsigned j = 0; j < 12; ++j)
        m.labels[j] = j + 1;
    for(unsigned j = 0; j < 8; ++j) {
        m.voices[j] = m.voices[0];
        m.voices[j].slice_count = 8;
        for(unsigned k = 0; k < 8; ++k)
            m.voices[j].slices[k] = (FlooperSlice){k * 2, 1, 440, 1};
        m.patterns[j] = (FlooperEventPattern){.first_event = j * 16, .event_count = 16};
        m.sections[j] = (FlooperSection){.cycles = UINT16_MAX, .pattern = j, .layer_mask = 255};
    }
    for(unsigned j = 0; j < 128; ++j)
        m.events[j] = (FlooperEvent){
            .offset_us = (j % 16) / 12 * 20,
            .velocity = 1,
            .count_index = (j % 16) % 12,
            .voice = j % 8,
            .layer = j % 8};
    compile_ok(&m, &s);
    assert(s.interval_count == 1024 && s.range_count == 96);
    assert(s.total_counts == UINT64_C(8) * UINT16_MAX * 12);
    ++cases;
    m.patterns[7].first_event = 111;
    m.patterns[7].event_count = 17;
    m.events[111].offset_us = 40;
    compile_error(&m, FlooperScheduleIntervalLimit);
    ++cases;
    /* Given repeated elapsed counts near u64; when located; then no wrap. */
    m = fixture();
    m.repeat = true;
    compile_ok(&m, &s);
    FlooperPosition p = {.elapsed_count = UINT64_MAX};
    assert(flooper_schedule_position(&s, &p) == FlooperScheduleOverflow);
    assert(p.count_start_us == 0 && p.elapsed_count == UINT64_MAX);
    p.elapsed_count = UINT64_MAX / 100;
    assert(flooper_schedule_position(&s, &p) == FlooperScheduleOverflow);
    p.elapsed_count = UINT64_MAX / 100 - 1;
    assert(flooper_schedule_position(&s, &p) == FlooperScheduleOk);
    ++cases;
    s.repeat = false;
    p.elapsed_count = 2;
    assert(flooper_schedule_position(&s, &p) == FlooperSchedulePositionEnd);
    ++cases;
    compile_error(NULL, FlooperScheduleInvalid);
    assert(flooper_schedule_compile(&m, NULL) == FlooperScheduleInvalid);
    assert(flooper_schedule_position(NULL, &p) == FlooperScheduleInvalid);
    assert(flooper_schedule_position(&s, NULL) == FlooperScheduleInvalid);
    ++cases;
    return cases;
}
