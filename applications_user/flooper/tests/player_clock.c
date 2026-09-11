#include "player_test.h"

static void long_grid(void) {
    /* Given a wrapped raw clock and delayed attacks; when 10001 cycles elapse;
     * then every beat/attack stays on the original ideal grid through forms. */
    uint64_t epoch = fake.tick;
    FlooperPlayer* p = player_ready();
    player_command(p, FlooperCommandToggle);
    for(uint64_t count = 0; count < 20002; ++count) {
        uint64_t us = count * 314667;
        fake_advance(epoch + player_tick(us));
        FlooperSnapshot s = player_snapshot(p);
        assert(s.state == FlooperStatePlaying && s.count_index == count % 2);
        assert(s.section_index == ((count + 2) % 6 < 2 ? 0 : 1));
        assert(s.cycle_index == ((count + 2) % 6 < 2 ? 0 : ((count + 2) % 6 - 2) / 2));
        size_t before = fake_events(FakeStart);
        fake_advance(epoch + player_tick(us + 20000));
        assert(fake_events(FakeStart) == before + 1);
        assert(fake.events[fake.event_count - 1].tick == epoch + player_tick(us + 20000));
        fake_advance(epoch + player_tick(us + 30000));
        assert(fake.events[fake.event_count - 2].kind == FakeStop);
        assert(fake.events[fake.event_count - 1].kind == FakeStart);
        fake_advance(epoch + player_tick(us + 40000));
        assert(!fake.sounding);
    }
    assert(fake.max_wait <= fake.hz / 10 && fake.max_wait > 0);
    flooper_player_free(p);
    player_case();
}
static void late_and_early(void) {
    /* Given a playing count; when waits wake early or after deadlines; then
     * no early tone, no expired attack, and only the live remainder sounds. */
    FlooperPlayer* p = player_ready();
    uint64_t epoch = fake.tick;
    player_command(p, FlooperCommandToggle);
    for(unsigned i = 0; i < 50; ++i)
        fake_wake();
    assert(fake_events(FakeStart) == 0 && player_snapshot(p).count_index == 0);
    fake_advance(epoch + player_tick(25000));
    assert(fake_events(FakeStart) == 1 && fake.events[fake.event_count - 1].frequency == 440);
    fake_advance(epoch + player_tick(40000));
    assert(fake_events(FakeStart) == 1 && !fake.sounding);
    fake_advance(epoch + player_tick(314667 + 40000));
    assert(fake_events(FakeStart) == 1 && player_snapshot(p).count_index == 1);
    fake_advance(epoch + player_tick(314667 * 1000ULL + 35000));
    assert(fake_events(FakeStart) == 2 && fake.events[fake.event_count - 1].frequency == 880);
    flooper_player_free(p);
    player_case();
}
void test_clock(void) {
    uint32_t hz = fake.hz;
    fake_reset(hz, UINT32_MAX - 10);
    long_grid();
    fake_reset(hz, 0);
    late_and_early();
    /* Given a boundary snapshot consuming its one-tick mutex budget; when that
     * expires a late tone; then no stale-now start is emitted afterwards. */
    fake_reset(hz, 0);
    FlooperPlayer* p = player_ready();
    player_command(p, FlooperCommandToggle);
    fake.publish_delay = 1;
    fake_advance(player_tick(314667 + 40000) - 1);
    assert(fake_events(FakeStart) == 0);
    flooper_player_free(p);
    player_case();
}
