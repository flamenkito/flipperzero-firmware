#include "schedule_test.h"
#include <stdio.h>

static FlooperPattern canonical, legacy;
static FlooperSchedule a, b;
static FlooperPatternWorkspace workspace;

static void load(const char* root, const char* relative, FlooperPattern* m) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE* file = fopen(path, "rb");
    assert(file);
    static char input[FLOOPER_JSON_BYTES + 1];
    size_t size = fread(input, 1, sizeof(input), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(flooper_pattern_parse(input, size, &workspace, m) == FlooperPatternOk);
}

static void equivalent(void) {
    compile_ok(&canonical, &a);
    compile_ok(&legacy, &b);
    assert(a.interval_count == b.interval_count && a.range_count == b.range_count);
    assert(a.cycle_us == b.cycle_us && a.form_us == b.form_us);
    assert(memcmp(a.sections, b.sections, sizeof(a.sections)) == 0);
    assert(memcmp(a.ranges, b.ranges, sizeof(a.ranges)) == 0);
    for(unsigned i = 0; i < a.interval_count; ++i) {
        assert(a.intervals[i].start_us == b.intervals[i].start_us);
        assert(a.intervals[i].end_us == b.intervals[i].end_us);
        assert(a.intervals[i].frequency_hz == b.intervals[i].frequency_hz);
        assert(fabsf(a.intervals[i].volume - b.intervals[i].volume) < 0.000001f);
    }
}

unsigned schedule_music(const char* root) {
    unsigned cases = 0;
    /* Given unchanged Bulerias files; when parsed and compiled; then their
     * complete schedules agree and independently specified musical spans hold. */
    load(root, "assets/flipper_bulerias_pattern_v2_1.json", &canonical);
    load(root, "tests/fixtures/bulerias_v1.json", &legacy);
    equivalent();
    assert(a.cycle_us == 1888002 && a.form_us == 37760040);
    assert(a.sections[0].start_us == 0 && a.sections[1].start_us == 7552008);
    assert(a.sections[2].start_us == 15104016);
    for(unsigned c = 0; c < 6; ++c)
        assert(a.ranges[c].interval_count == 0);
    ++cases;
    const float velocities[] = {0.35f, 0.55f, 0.65f, 1, 0.65f, 1};
    const uint32_t starts[] = {0, 9000, 19000};
    const uint32_t ends[] = {8000, 18000, 31000};
    const float frequencies[] = {2850, 2250, 1750}, gains[] = {1, 0.85f, 0.7f};
    for(unsigned c = 0; c < 6; ++c) {
        const FlooperCountRange* r = &a.ranges[a.sections[1].first_range + c];
        assert(r->interval_count == 3);
        for(unsigned j = 0; j < 3; ++j) {
            const FlooperInterval* s = &a.intervals[r->first_interval + j];
            assert(s->start_us == starts[j] && s->end_us == ends[j]);
            assert(s->frequency_hz == frequencies[j]);
            assert(fabsf(s->volume - 0.65f * velocities[c] * gains[j]) < 0.000001f);
        }
        ++cases;
    }
    const FlooperInterval golden[] = {
        {0, 10000, 330, 0.65f * 0.55f * 0.85f},
        {10000, 22000, 260, 0.65f * 0.55f},
        {22000, 38000, 205, 0.65f * 0.55f * 0.85f},
        {42000, 50000, 2850, 0.65f * 0.65f},
        {51000, 60000, 2250, 0.65f * 0.65f * 0.85f},
        {61000, 73000, 1750, 0.65f * 0.65f * 0.7f},
        {157334, 166334, 1450, 0.65f * 0.55f},
        {167334, 173334, 2050, 0.65f * 0.55f * 0.7f},
    };
    const FlooperCountRange* r = &a.ranges[a.sections[2].first_range + 2];
    assert(r->interval_count == 8);
    for(unsigned j = 0; j < 8; ++j) {
        const FlooperInterval* s = &a.intervals[r->first_interval + j];
        assert(s->start_us == golden[j].start_us && s->end_us == golden[j].end_us);
        assert(s->frequency_hz == golden[j].frequency_hz);
        assert(fabsf(s->volume - golden[j].volume) < 0.000001f);
    }
    ++cases;
    /* Given corrected Tangos and the old draft; when compiled; then all 16
     * attacks preserve time and every event/voice property without rotation. */
    load(root, "assets/flipper_tangos_pattern_v2_1.json", &canonical);
    load(root, "tests/fixtures/tangos_v2_draft.json", &legacy);
    equivalent();
    assert(a.cycle_us == 2879276 && b.cycle_us == 2879276);
    assert(canonical.event_count == 16 && legacy.event_count == 16);
    assert(memcmp(canonical.voices, legacy.voices, sizeof(canonical.voices)) == 0);
    assert(memcmp(canonical.events, legacy.events, sizeof(canonical.events)) == 0);
    assert(legacy.warning_cycle_mismatch && legacy.reported_cycle_us == 2879274);
    ++cases;
    const uint32_t attacks[] = {
        0,
        216000,
        388000,
        562000,
        0,
        203000,
        390000,
        572000,
        0,
        202000,
        389000,
        563000,
        0,
        195000,
        392000,
        567000};
    const uint8_t mapped[] = {4, 1, 2, 3};
    for(unsigned j = 0; j < 16; ++j) {
        const FlooperEvent* e = &canonical.events[j];
        assert(e->count_index == j / 4 && e->offset_us == attacks[j]);
        assert(canonical.labels[e->count_index] == mapped[j / 4]);
        assert(legacy.labels[legacy.events[j].count_index] == j / 4 + 1);
        uint64_t old_time =
            (uint64_t)legacy.events[j].count_index * 719819 + legacy.events[j].offset_us;
        assert(old_time == (uint64_t)e->count_index * 719819 + attacks[j]);
        const FlooperCountRange* range = &a.ranges[e->count_index];
        bool found = false;
        for(unsigned k = 0; k < range->interval_count; ++k) {
            const FlooperInterval* span = &a.intervals[range->first_interval + k];
            if(span->start_us == attacks[j]) {
                const FlooperSlice* slice = &canonical.voices[e->voice].slices[0];
                assert(span->end_us == attacks[j] + slice->duration_us);
                assert(span->frequency_hz == slice->frequency_hz);
                assert(fabsf(span->volume - 0.65f * e->velocity * slice->gain) < 0.000001f);
                found = true;
            }
        }
        assert(found);
        ++cases;
    }
    return cases;
}
