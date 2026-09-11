#include "player_test.h"

void test_quit(void) {
    uint32_t hz = fake.hz;
    /* Given each uninterruptible Storage stage and a full command queue; when
     * exit is requested; then it is immediately sticky and joins after release. */
    for(FakeStage stage = FakeStageOpen; stage <= FakeStageClose; ++stage) {
        fake_reset(hz, 0);
        fake_document(player_document);
        fake.block_stage = stage;
        FlooperPlayer* p = flooper_player_alloc(0);
        fake_wait_stage(stage);
        assert(player_snapshot(p).state == FlooperStateLoading);
        unsigned accepted = 0;
        while(flooper_player_send(p, (FlooperCommand){FlooperCommandToggle, 0})) {
            assert(++accepted <= FLOOPER_COMMAND_CAPACITY);
        }
        assert(accepted == FLOOPER_COMMAND_CAPACITY);
        flooper_player_request_exit(p);
        assert(flooper_player_exit_requested(p) && !fake.exited);
        assert(!flooper_player_send(p, (FlooperCommand){FlooperCommandRestart, 0}));
        flooper_player_request_exit(p);
        fake_unblock();
        assert(player_snapshot(p).state == FlooperStateQuitting);
        assert(fake_events(FakeStart) == 0 && fake.opens == fake.closes);
        flooper_player_free(p);
        assert(fake.joined);
        player_case();
    }
    /* Given each non-loading state; when exit interrupts it repeatedly; then
     * the worker alone releases any ownership before join and resource free. */
    for(unsigned state = 0; state < 4; ++state) {
        fake_reset(hz, 0);
        FlooperPlayer* p = player_ready();
        switch(state) {
        case 0:
            break;
        case 1:
            player_command(p, FlooperCommandToggle);
            break;
        case 2:
            fake.busy = true;
            player_command(p, FlooperCommandToggle);
            break;
        case 3:
            fake.open_fail = true;
            player_command(p, FlooperCommandLoadSelected);
            break;
        }
        assert(flooper_player_send(p, (FlooperCommand){FlooperCommandQuit, 0}));
        assert(flooper_player_exit_requested(p));
        fake_sync();
        flooper_player_free(p);
        assert(fake_events(FakeRelease) == (state == 1));
        player_case();
    }
    /* Given Toggle queued during Loading; when loading completes; then stale
     * commands cannot turn a newly selected paused document into playback. */
    fake_reset(hz, 0);
    fake_document(player_document);
    fake.block_stage = FakeStageOpen;
    FlooperPlayer* p = flooper_player_alloc(0);
    fake_wait_stage(FakeStageOpen);
    assert(flooper_player_send(p, (FlooperCommand){FlooperCommandToggle, 0}));
    fake_unblock();
    assert(player_snapshot(p).state == FlooperStatePaused && fake_events(FakeAcquire) == 0);
    flooper_player_free(p);
    player_case();
    /* Given a full GUI-like input queue and a worker not yet scheduled; when
     * Long Back uses the direct sticky API; then neither queue can lose exit. */
    fake_reset(hz, 0);
    fake_document(player_document);
    fake.hold_worker = true;
    p = flooper_player_alloc(0);
    FuriMessageQueue* input = furi_message_queue_alloc(2, sizeof(unsigned));
    unsigned key = 1;
    assert(furi_message_queue_put(input, &key, 0) == FuriStatusOk);
    assert(furi_message_queue_put(input, &key, 0) == FuriStatusOk);
    assert(furi_message_queue_put(input, &key, 0) == FuriStatusErrorResource);
    for(unsigned i = 0; i < FLOOPER_COMMAND_CAPACITY; ++i)
        assert(flooper_player_send(p, (FlooperCommand){FlooperCommandToggle, 0}));
    flooper_player_request_exit(p);
    assert(flooper_player_exit_requested(p));
    fake_unblock();
    flooper_player_free(p);
    assert(fake.opens == 0 && fake_events(FakeAcquire) == 0);
    furi_message_queue_free(input);
    player_case();
}
