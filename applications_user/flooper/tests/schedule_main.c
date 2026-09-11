#include "schedule_test.h"
#include <stdio.h>

static unsigned arbitration(void) {
    unsigned cases = 0;
    /* Given overlapping events; when compiled; then each ordering key decides. */
    for(unsigned key = 0; key < 4; ++key) {
        FlooperPattern m = fixture();
        static FlooperSchedule s;
        m.voice_count = 2;
        m.voices[1] = m.voices[0];
        m.voices[1].slices[0].frequency_hz = 880;
        m.event_count = m.patterns[0].event_count = 2;
        m.events[1] = m.events[0];
        m.events[1].voice = 1;
        if(key == 0) {
            m.voices[1].priority = 1;
            m.events[1].velocity = 0.5;
        }
        if(key == 1) m.events[0].velocity = 0.5;
        if(key == 3) {
            m.event_count = m.patterns[0].event_count = 1;
            m.voices[0].slice_count = 2;
            m.voices[0].slices[1] = m.voices[1].slices[0];
        }
        compile_ok(&m, &s);
        assert(s.interval_count == 1);
        assert(s.intervals[0].frequency_hz == (key < 2 ? 880 : 440));
        ++cases;
    }
    /* Given nested or partial winners; when compiled; then losers retain life. */
    for(unsigned partial = 0; partial < 2; ++partial) {
        FlooperPattern m = fixture();
        static FlooperSchedule s;
        m.voices[0].slice_count = 2;
        m.voices[0].slices[0].gain = 0.5;
        m.voices[0].slices[1] = (FlooperSlice){20, partial ? 80 : 20, 880, 1};
        compile_ok(&m, &s);
        assert(s.interval_count == (partial ? 2 : 3));
        assert(s.intervals[0].start_us == 0 && s.intervals[0].end_us == 20);
        assert(s.intervals[1].start_us == 20 && s.intervals[1].end_us == (partial ? 100 : 40));
        if(!partial) {
            assert(s.intervals[2].start_us == 40 && s.intervals[2].end_us == 80);
            assert(s.intervals[2].frequency_hz == 440);
        }
        ++cases;
    }
    return cases;
}

static unsigned boundaries(void) {
    unsigned cases = 0;
    /* Given adjacent spans; when compiled; then only equal sound coalesces. */
    for(unsigned variant = 0; variant < 4; ++variant) {
        FlooperPattern m = fixture();
        static FlooperSchedule s;
        m.voices[0].slice_count = 2;
        m.voices[0].slices[0].duration_us = 20;
        m.voices[0].slices[1] = (FlooperSlice){20, 20, 440, 1};
        if(variant == 1) m.voices[0].slices[1].frequency_hz = 880;
        if(variant == 2) m.voices[0].slices[1].gain = 0.5;
        if(variant == 3) m.voices[0].slices[1].offset_us = 21;
        compile_ok(&m, &s);
        assert(s.interval_count == (variant ? 2 : 1));
        assert(s.intervals[0].start_us == 0);
        assert(s.intervals[s.interval_count - 1].end_us == (variant == 3 ? 41 : 40));
        ++cases;
    }
    /* Given zero or out-of-unit finite products; when compiled; then clamp. */
    for(unsigned variant = 0; variant < 6; ++variant) {
        FlooperPattern m = fixture();
        static FlooperSchedule s;
        if(variant == 0) m.master_volume = 0;
        if(variant == 1) m.events[0].velocity = 0;
        if(variant == 2) m.voices[0].slices[0].gain = 0;
        if(variant == 3) m.master_volume = -1;
        if(variant == 4) m.master_volume = 2;
        if(variant == 5) {
            m.master_volume = 0.5;
            m.events[0].velocity = 0.5;
            m.voices[0].slices[0].gain = 0.5;
        }
        compile_ok(&m, &s);
        assert(s.interval_count == (variant < 4 ? 0 : 1));
        if(variant >= 4) assert(s.intervals[0].volume == (variant == 4 ? 1.0f : 0.125f));
        ++cases;
    }
    return cases;
}

static unsigned positions(void) {
    FlooperPattern m = fixture();
    static FlooperSchedule s;
    /* Given reordered labels and repeated sections; when compiled; then reuse
     * local ranges while descriptors retain generic section/cycle positions. */
    m.count_count = 12;
    m.cycle_us = 1200;
    for(unsigned i = 0; i < 12; ++i)
        m.labels[i] = 24 - i;
    m.events[0].count_index = 11;
    m.section_count = 3;
    m.sections[1] = m.sections[0];
    m.sections[2] = m.sections[0];
    m.sections[2].layer_mask = 0;
    m.repeat = true;
    compile_ok(&m, &s);
    assert(s.range_count == 24 && s.interval_count == 1 && s.form_us == 7200);
    assert(s.sections[1].first_range == s.sections[0].first_range);
    assert(s.intervals[0].start_us == 0 && s.intervals[0].end_us == 80);
    for(uint64_t i = 0; i < 144; ++i) {
        FlooperPosition p = {.elapsed_count = i};
        assert(flooper_schedule_position(&s, &p) == FlooperScheduleOk);
        assert(p.form_index == i / 72 && p.section_index == (i % 72) / 24);
        assert(p.cycle_index == (i % 24) / 12 && p.count_index == i % 12);
        assert(p.count_start_us == i * 100 && p.label == 24 - i % 12);
    }
    return 1;
}

static unsigned layers(void) {
    /* Given competing layers and a gapped priority voice; when compiled;
     * then masks select events and the quiet voice fills the actual gap. */
    for(unsigned mask = 0; mask < 4; ++mask) {
        FlooperPattern m = fixture();
        static FlooperSchedule s;
        m.layer_count = m.voice_count = 2;
        m.event_count = m.patterns[0].event_count = 2;
        m.events[1] = m.events[0];
        m.events[1].voice = m.events[1].layer = 1;
        m.voices[1] = m.voices[0];
        m.voices[1].priority = 1;
        m.voices[1].slice_count = 2;
        m.voices[1].slices[0] = (FlooperSlice){0, 20, 880, 1};
        m.voices[1].slices[1] = (FlooperSlice){40, 40, 880, 1};
        m.sections[0].layer_mask = mask;
        compile_ok(&m, &s);
        assert(s.interval_count == mask);
        if(mask == 1) assert(s.intervals[0].frequency_hz == 440);
        if(mask == 2) assert(s.intervals[1].start_us == 40);
        if(mask == 3) {
            assert(s.intervals[1].start_us == 20 && s.intervals[1].end_us == 40);
            assert(s.intervals[1].frequency_hz == 440);
        }
    }
    return 4;
}

int main(int argc, char** argv) {
    if(argc != 2) return 2;
    unsigned cases = arbitration() + boundaries() + positions() + layers() + schedule_bounds() +
                     schedule_music(argv[1]);
    printf("schedule: PASS cases=%u failures=0\n", cases);
    return 0;
}
