#include "player_test.h"

void test_loading(const char* root) {
    uint32_t hz = fake.hz;
    /* Given a valid old document; when replacement fails at each boundary;
     * then generation changes, old labels vanish, and Toggle cannot play it. */
    for(unsigned failure = 0; failure < 10; ++failure) {
        fake_reset(hz, 0);
        FlooperPlayer* p = player_ready();
        switch(failure) {
        case 0:
            fake.open_fail = true;
            break;
        case 1:
            fake.short_read = true;
            break;
        case 2:
            fake.close_fail = true;
            break;
        case 3:
            fake.document_size = 16385;
            break;
        case 4:
            fake_document("{\"schema_version\":2,\"schema_version\":1}");
            break;
        case 5:
            fake_document("{}");
            break;
        case 6:
            fake.heap_fail_after = 0;
            break;
        case 7:
            fake.heap_fail_after = 1;
            break;
        case 8:
            fake.heap_fail_after = 2;
            break;
        case 9:
            fake.record_missing = true;
            break;
        }
        assert(flooper_player_send(p, (FlooperCommand){FlooperCommandLoadSelected, 1}));
        fake_sync();
        FlooperSnapshot s = player_snapshot(p);
        assert(s.state == FlooperStatePatternError && s.generation == 2 && s.selected_index == 1);
        assert(s.error != FlooperPlayerOk && s.pulse_us == 0 && s.display_name[0] == 0);
        assert(s.count_count == 0 && s.accent_count == 0);
        player_command(p, FlooperCommandToggle);
        assert(fake_events(FakeAcquire) == 0 && !fake.owned);
        assert(fake.file_count == 0 && fake.record_count == 0 && fake.opens == fake.closes);
        fake.open_fail = fake.short_read = fake.close_fail = fake.record_missing = false;
        fake.heap_fail_after = -1;
        fake_document(player_document);
        player_command(p, FlooperCommandLoadSelected);
        assert(
            player_snapshot(p).state == FlooperStatePaused && player_snapshot(p).generation == 3);
        flooper_player_free(p);
        player_case();
    }
    /* Given the actual assets and legacy adapter; when loaded through Storage;
     * then all are paused and the legacy timing warning is logged once. */
    const char* paths[] = {
        "assets/flipper_bulerias_pattern_v2_1.json",
        "assets/flipper_tangos_pattern_v2_1.json",
        "tests/fixtures/tangos_v2_draft.json"};
    for(unsigned i = 0; i < 3; ++i) {
        fake_reset(hz, 0);
        fake_read_document(root, paths[i]);
        FlooperPlayer* p = flooper_player_alloc(i != 0);
        fake_sync();
        assert(player_snapshot(p).state == FlooperStatePaused && fake.logs == (i == 2));
        player_command(p, FlooperCommandToggle);
        assert(fake.owned && player_snapshot(p).state == FlooperStatePlaying);
        flooper_player_free(p);
        player_case();
    }
    fake_reset(hz, 0);
    fake.heap_fail_after = 0;
    assert(flooper_player_alloc(0) == NULL);
    player_case();
    fake_reset(hz, 0);
    fake_document(player_document);
    FlooperPlayer* invalid = flooper_player_alloc(255);
    fake_sync();
    assert(player_snapshot(invalid).error == FlooperPlayerSelection && fake.opens == 0);
    flooper_player_free(invalid);
    player_case();
}
