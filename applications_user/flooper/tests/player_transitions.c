#include "player_test.h"

static void resume_restart(void) {
    /* Given count one; when pause/resume and restart are commanded; then
     * resume attacks from its boundary and restart obeys worker state. */
    FlooperPlayer* p = player_ready();
    FlooperSnapshot original = player_snapshot(p);
    assert(original.section_index == 1 && original.cycle_index == 0 && original.count_index == 0);
    assert(original.generation == 1 && original.pulse_us == 314667);
    assert(
        strcmp(original.display_name, "GRID") == 0 &&
        strcmp(original.section_name, "SECOND") == 0);
    assert(original.count_labels[0] == 6 && original.count_labels[1] == 1);
    assert(original.accent_count == 1 && original.accent_labels[0] == 6);
    original.display_name[0] = '!';
    assert(player_snapshot(p).display_name[0] == 'G');
    player_command(p, FlooperCommandRestart);
    assert(player_snapshot(p).state == FlooperStatePaused && fake_events(FakeAcquire) == 0);
    player_command(p, FlooperCommandToggle);
    fake_advance(player_tick(314667 + 25000));
    player_command(p, FlooperCommandToggle);
    assert(player_snapshot(p).state == FlooperStatePaused && !fake.owned);
    assert(player_snapshot(p).count_index == 1);
    uint64_t resume = fake.tick;
    player_command(p, FlooperCommandToggle);
    size_t starts = fake_events(FakeStart);
    fake_advance(resume + player_tick(20000));
    assert(fake_events(FakeStart) == starts + 1);
    assert(fake.events[fake.event_count - 1].frequency == 440);
    player_command(p, FlooperCommandRestart);
    assert(player_snapshot(p).state == FlooperStatePlaying && player_snapshot(p).count_index == 0);
    assert(player_snapshot(p).section_index == 1 && !fake.sounding);
    uint64_t restart = fake.tick;
    fake_advance(restart + player_tick(20000));
    assert(fake.sounding && fake.events[fake.event_count - 1].frequency == 440);
    flooper_player_free(p);
    assert(fake.events[fake.event_count - 5].kind == FakeStop);
    assert(fake.events[fake.event_count - 4].kind == FakeRelease);
    assert(fake.events[fake.event_count - 3].kind == FakeExit);
    assert(fake.events[fake.event_count - 2].kind == FakeJoin);
    assert(fake.events[fake.event_count - 1].kind == FakeFree);
    player_case();
}
static void finite(void) {
    /* Given a finite form starting in section one; when its last count ends;
     * then ownership is released and next Toggle begins at the configured start. */
    char document[2048];
    snprintf(document, sizeof(document), "%s", player_document);
    char* repeat = strstr(document, "\"repeat\":true");
    assert(repeat);
    memmove(repeat + 14, repeat + 13, strlen(repeat + 13) + 1);
    memcpy(repeat + 9, "false", 5);
    fake_document(document);
    FlooperPlayer* p = flooper_player_alloc(0);
    fake_sync();
    assert(player_snapshot(p).state == FlooperStatePaused);
    player_command(p, FlooperCommandToggle);
    fake_advance(player_tick(4ULL * 314667));
    FlooperSnapshot s = player_snapshot(p);
    assert(s.state == FlooperStatePaused && s.section_index == 1 && s.count_index == 0);
    assert(!fake.owned && fake_events(FakeRelease) == 1);
    player_command(p, FlooperCommandToggle);
    assert(player_snapshot(p).state == FlooperStatePlaying);
    flooper_player_free(p);
    player_case();
}
static void busy_retry(void) {
    /* Given another speaker owner; when Toggle retries after it releases;
     * then Busy is recoverable without stop/release of someone else's speaker. */
    FlooperPlayer* p = player_ready();
    fake.busy = true;
    player_command(p, FlooperCommandToggle);
    assert(player_snapshot(p).state == FlooperStateSpeakerBusy);
    assert(fake_events(FakeRelease) == 0 && fake_events(FakeStop) == 0);
    fake.busy = false;
    player_command(p, FlooperCommandToggle);
    assert(player_snapshot(p).state == FlooperStatePlaying);
    unsigned opens = fake.opens;
    player_command(p, FlooperCommandLoadSelected);
    assert(fake.opens == opens && player_snapshot(p).generation == 1);
    flooper_player_free(p);
    player_case();
}
void test_transitions(void) {
    uint32_t hz = fake.hz;
    fake_reset(hz, 0);
    resume_restart();
    fake_reset(hz, 0);
    finite();
    fake_reset(hz, 0);
    busy_retry();
    /* Given a command waking the worker after a count boundary; when paused;
     * then resume uses that current count, without sounding an overdue attack. */
    fake_reset(hz, 0);
    FlooperPlayer* p = player_ready();
    player_command(p, FlooperCommandToggle);
    pthread_mutex_lock(&fake.lock);
    fake.tick = player_tick(314667 + 25000);
    pthread_mutex_unlock(&fake.lock);
    player_command(p, FlooperCommandToggle);
    assert(player_snapshot(p).state == FlooperStatePaused);
    assert(player_snapshot(p).count_index == 1 && fake_events(FakeStart) == 0);
    flooper_player_free(p);
    player_case();
}
