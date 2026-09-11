#pragma once
#include "fakes/furi.h"
#include <pthread.h>
#include <stdio.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

typedef enum {
    FakeAcquire,
    FakeStart,
    FakeStop,
    FakeRelease,
    FakeExit,
    FakeJoin,
    FakeFree
} FakeKind;
typedef struct {
    FakeKind kind;
    uint64_t tick;
    float frequency;
} FakeEvent;
typedef enum {
    FakeStageNone,
    FakeStageOpen,
    FakeStageRead,
    FakeStageSize,
    FakeStageClose
} FakeStage;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t tick, kick, observed;
    uint32_t hz, max_wait;
    bool waiting, exited, joined, owned, sounding, busy;
    bool hold_worker, open_fail, short_read, close_fail, record_missing;
    FakeStage block_stage, entered_stage;
    unsigned record_count, file_count, opens, closes, reads, logs;
    int heap_fail_after;
    uint32_t publish_delay;
    bool app_mode, worker_started;
    unsigned mutex_allocs, queue_allocs, gui_records;
    void (*app_step)(void);
    void (*exit_observer)(void);
    const char* document;
    const char* expected_path;
    size_t document_size;
    FakeEvent events[200000];
    size_t event_count;
    size_t event_counts[7];
} Fake;
extern Fake fake;
extern _Thread_local bool fake_worker;
extern _Thread_local unsigned fake_locks;
void fake_reset(uint32_t hz, uint64_t tick);
void fake_sync(void);
void fake_advance(uint64_t tick);
void fake_wake(void);
void fake_unblock(void);
void fake_wait_stage(FakeStage stage);
void fake_stage(FakeStage stage);
void fake_event(FakeKind kind, float frequency);
void fake_document(const char* document);
void fake_read_document(const char* root, const char* relative);
size_t fake_events(FakeKind kind);
void test_clock(void);
void test_transitions(void);
void test_loading(const char* root);
void test_quit(void);
void test_limits(void);
extern unsigned player_cases;
void player_case(void);
