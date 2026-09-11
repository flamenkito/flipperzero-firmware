#include "player_test.h"

const char player_document[] =
    "{\"schema_version\":2,\"schema_revision\":1,\"name\":\"test\",\"display_name\":\"GRID\","
    "\"timebase\":{\"count_labels\":[6,1],\"pulse_us\":314667},"
    "\"layers\":[\"tone\"],\"voices\":[{\"id\":\"v\",\"priority\":1,\"slices\":["
    "{\"offset_us\":0,\"duration_us\":10000,\"frequency_hz\":440,\"gain\":1},"
    "{\"offset_us\":10000,\"duration_us\":10000,\"frequency_hz\":880,\"gain\":1}]}],"
    "\"patterns\":[{\"id\":\"p\",\"events\":["
    "{\"count\":6,\"offset_us\":20000,\"voice\":\"v\",\"layer\":\"tone\",\"velocity\":1},"
    "{\"count\":1,\"offset_us\":20000,\"voice\":\"v\",\"layer\":\"tone\",\"velocity\":1}]}],"
    "\"sections\":[{\"name\":\"FIRST\",\"pattern_id\":\"p\",\"cycles\":1,\"layers\":[\"tone\"]},"
    "{\"name\":\"SECOND\",\"pattern_id\":\"p\",\"cycles\":2,\"layers\":[\"tone\"]}],"
    "\"loop\":{\"start_section\":1,\"repeat\":true},"
    "\"render\":{\"master_volume\":0.5,\"monophonic\":true,"
    "\"overlap_policy\":\"higher_priority_then_louder\"},\"ui\":{\"accent_counts\":[6]}}";

unsigned player_cases;
void player_case(void) {
    player_cases++;
}
FlooperSnapshot player_snapshot(FlooperPlayer* player) {
    FlooperSnapshot snapshot;
    assert(flooper_player_snapshot(player, &snapshot));
    return snapshot;
}
FlooperPlayer* player_ready(void) {
    fake_document(player_document);
    FlooperPlayer* player = flooper_player_alloc(0);
    assert(player);
    fake_sync();
    assert(player_snapshot(player).state == FlooperStatePaused);
    return player;
}
void player_command(FlooperPlayer* player, FlooperCommandType type) {
    assert(flooper_player_send(player, (FlooperCommand){.type = type}));
    fake_sync();
}
uint64_t player_tick(uint64_t microseconds) {
    /* Independent oracle: test durations fit the simple product, unlike the
     * production full-lifetime clock which must split before multiplication. */
    return (microseconds * fake.hz + 500000) / 1000000;
}
