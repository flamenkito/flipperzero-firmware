#include "player_test.h"
#include <stdarg.h>

static void append(char* buffer, size_t* used, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer + *used, FLOOPER_JSON_BYTES - *used, format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < FLOOPER_JSON_BYTES - *used);
    *used += (size_t)length;
}
void test_limits(void) {
    /* Given valid JSON with eight distinct section masks; when compiled ranges
     * exceed 1024 intervals; then the adapter exposes a typed compile error. */
    char* json = malloc(FLOOPER_JSON_BYTES);
    assert(json);
    size_t used = 0;
    append(
        json,
        &used,
        "{\"schema_version\":2,\"schema_revision\":1,\"name\":\"limit\","
        "\"display_name\":\"LIMIT\",\"timebase\":{\"count_labels\":[1,2,3,4,5,6,7,8,9,10,11,12],"
        "\"pulse_us\":314667},\"layers\":[\"a\",\"b\",\"c\",\"d\"],"
        "\"voices\":[{\"id\":\"v\",\"priority\":1,\"slices\":[");
    for(unsigned i = 0; i < 8; ++i)
        append(
            json,
            &used,
            "%s{\"offset_us\":%u,\"duration_us\":1000,\"frequency_hz\":440,\"gain\":1}",
            i ? "," : "",
            i * 2000);
    append(json, &used, "]}],\"patterns\":[{\"id\":\"p\",\"events\":[");
    for(unsigned i = 0; i < 24; ++i)
        append(
            json,
            &used,
            "%s{\"count\":%u,\"offset_us\":%u,\"voice\":\"v\",\"layer\":\"a\",\"velocity\":1}",
            i ? "," : "",
            i / 2 + 1,
            (i % 2) * 100000);
    append(json, &used, "]}],\"sections\":[");
    for(unsigned mask = 0; mask < 8; ++mask) {
        append(
            json,
            &used,
            "%s{\"name\":\"S%u\",\"pattern_id\":\"p\",\"cycles\":1,\"layers\":[\"a\"",
            mask ? "," : "",
            mask);
        for(unsigned bit = 0; bit < 3; ++bit)
            if(mask & (1U << bit)) append(json, &used, ",\"%c\"", 'b' + bit);
        append(json, &used, "]}");
    }
    append(
        json,
        &used,
        "],\"loop\":{\"start_section\":0,\"repeat\":true},"
        "\"render\":{\"master_volume\":0.5,\"monophonic\":true,"
        "\"overlap_policy\":\"higher_priority_then_louder\"},\"ui\":{\"accent_counts\":[1]}}");
    fake_reset(fake.hz, 0);
    fake_document(json);
    FlooperPlayer* p = flooper_player_alloc(0);
    fake_sync();
    FlooperSnapshot s = player_snapshot(p);
    assert(s.state == FlooperStatePatternError && s.error == FlooperPlayerCompile);
    assert(
        s.schedule_error == FlooperScheduleIntervalLimit && s.pattern_error == FlooperPatternOk);
    assert(s.pulse_us == 0 && fake_events(FakeAcquire) == 0);
    flooper_player_free(p);
    free(json);
    player_case();
}
