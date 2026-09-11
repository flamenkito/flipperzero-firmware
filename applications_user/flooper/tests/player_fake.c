#include "player_fake.h"

Fake fake = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
_Thread_local bool fake_worker;
_Thread_local unsigned fake_locks;
struct FuriMutex {
    pthread_mutex_t mutex;
    bool ui;
};
struct FuriThread {
    pthread_t id;
    FuriThreadCallback callback;
    void* context;
};
struct FuriMessageQueue {
    uint32_t capacity, size, count, first;
    bool ui;
    unsigned char data[];
};

void fake_reset(uint32_t hz, uint64_t tick) {
    assert(fake.record_count == 0 && fake.file_count == 0 && !fake.owned);
    memset((char*)&fake + offsetof(Fake, tick), 0, sizeof(fake) - offsetof(Fake, tick));
    fake.hz = hz;
    fake.tick = tick;
    fake.heap_fail_after = -1;
}
void fake_sync(void) {
    pthread_mutex_lock(&fake.lock);
    while(!fake.exited && !(fake.waiting && fake.observed == fake.kick))
        pthread_cond_wait(&fake.changed, &fake.lock);
    pthread_mutex_unlock(&fake.lock);
}
void fake_advance(uint64_t tick) {
    pthread_mutex_lock(&fake.lock);
    assert(tick >= fake.tick);
    fake.tick = tick;
    fake.kick++;
    pthread_cond_broadcast(&fake.changed);
    pthread_mutex_unlock(&fake.lock);
    fake_sync();
}
void fake_wake(void) {
    fake_advance(fake.tick);
}
void fake_unblock(void) {
    pthread_mutex_lock(&fake.lock);
    fake.block_stage = FakeStageNone;
    fake.hold_worker = false;
    fake.kick++;
    pthread_cond_broadcast(&fake.changed);
    pthread_mutex_unlock(&fake.lock);
    fake_sync();
}
void fake_wait_stage(FakeStage stage) {
    pthread_mutex_lock(&fake.lock);
    while(fake.entered_stage != stage)
        pthread_cond_wait(&fake.changed, &fake.lock);
    pthread_mutex_unlock(&fake.lock);
}
void fake_stage(FakeStage stage) {
    assert(fake_worker && !fake.owned);
    pthread_mutex_lock(&fake.lock);
    fake.entered_stage = stage;
    pthread_cond_broadcast(&fake.changed);
    while(fake.block_stage == stage)
        pthread_cond_wait(&fake.changed, &fake.lock);
    pthread_mutex_unlock(&fake.lock);
}
void fake_event(FakeKind kind, float frequency) {
    assert(fake.event_count < sizeof(fake.events) / sizeof(fake.events[0]));
    fake.events[fake.event_count++] = (FakeEvent){kind, fake.tick, frequency};
    fake.event_counts[kind]++;
}
size_t fake_events(FakeKind kind) {
    return fake.event_counts[kind];
}
FuriMutex* furi_mutex_alloc(FuriMutexType type) {
    assert(type == FuriMutexTypeNormal);
    FuriMutex* result = malloc(sizeof(*result));
    assert(result && pthread_mutex_init(&result->mutex, NULL) == 0);
    result->ui = fake.app_mode && fake.mutex_allocs++ == 0;
    return result;
}
void furi_mutex_free(FuriMutex* mutex) {
    assert(mutex->ui || fake.joined);
    assert(pthread_mutex_destroy(&mutex->mutex) == 0);
    free(mutex);
}
FuriStatus furi_mutex_acquire(FuriMutex* mutex, uint32_t timeout) {
    assert(timeout <= 1 || (mutex->ui && timeout == FuriWaitForever));
    if(fake_worker && fake.publish_delay) {
        pthread_mutex_lock(&fake.lock);
        fake.tick += fake.publish_delay;
        fake.publish_delay = 0;
        pthread_mutex_unlock(&fake.lock);
    }
    int status = timeout == FuriWaitForever ? pthread_mutex_lock(&mutex->mutex) :
                                              pthread_mutex_trylock(&mutex->mutex);
    if(status == 0) fake_locks++;
    return status == 0 ? FuriStatusOk : FuriStatusErrorResource;
}
FuriStatus furi_mutex_release(FuriMutex* mutex) {
    assert(pthread_mutex_unlock(&mutex->mutex) == 0);
    assert(fake_locks);
    fake_locks--;
    return FuriStatusOk;
}
FuriMessageQueue* furi_message_queue_alloc(uint32_t capacity, uint32_t size) {
    assert(capacity > 0 && capacity <= 16);
    FuriMessageQueue* q = calloc(1, sizeof(*q) + capacity * size);
    assert(q);
    q->capacity = capacity;
    q->size = size;
    q->ui = fake.app_mode && fake.queue_allocs++ == 0;
    return q;
}
void furi_message_queue_free(FuriMessageQueue* q) {
    assert(q->ui || (fake.joined && fake.exited));
    if(!q->ui) fake_event(FakeFree, 0);
    free(q);
}
FuriStatus furi_message_queue_put(FuriMessageQueue* q, const void* data, uint32_t timeout) {
    assert(timeout == 0);
    if(!q->ui && fake.exit_observer) fake.exit_observer();
    pthread_mutex_lock(&fake.lock);
    bool room = q->count < q->capacity;
    if(room) {
        memcpy(q->data + ((q->first + q->count) % q->capacity) * q->size, data, q->size);
        q->count++;
        fake.kick++;
        pthread_cond_broadcast(&fake.changed);
    }
    pthread_mutex_unlock(&fake.lock);
    return room ? FuriStatusOk : FuriStatusErrorResource;
}
FuriStatus furi_message_queue_get(FuriMessageQueue* q, void* data, uint32_t timeout) {
    assert((fake_worker || q->ui) && timeout <= fake.hz / 10);
    if(q->ui && timeout) {
        assert(timeout <= fake.hz / 50);
        if(fake.app_step) fake.app_step();
    }
    pthread_mutex_lock(&fake.lock);
    if(timeout > fake.max_wait) fake.max_wait = timeout;
    if(!q->ui && !q->count && timeout) {
        fake.waiting = true;
        fake.observed = fake.kick;
        pthread_cond_broadcast(&fake.changed);
        while(fake.observed == fake.kick)
            pthread_cond_wait(&fake.changed, &fake.lock);
        fake.waiting = false;
    }
    bool ready = q->count != 0;
    if(ready) {
        memcpy(data, q->data + q->first * q->size, q->size);
        q->first = (q->first + 1) % q->capacity;
        q->count--;
    }
    pthread_mutex_unlock(&fake.lock);
    return ready ? FuriStatusOk : FuriStatusErrorTimeout;
}
FuriStatus furi_message_queue_reset(FuriMessageQueue* q) {
    pthread_mutex_lock(&fake.lock);
    q->count = q->first = 0;
    pthread_mutex_unlock(&fake.lock);
    return FuriStatusOk;
}
static void* fake_run(void* context) {
    FuriThread* thread = context;
    fake_worker = true;
    pthread_mutex_lock(&fake.lock);
    while(fake.hold_worker)
        pthread_cond_wait(&fake.changed, &fake.lock);
    pthread_mutex_unlock(&fake.lock);
    assert(thread->callback(thread->context) == 0);
    pthread_mutex_lock(&fake.lock);
    assert(!fake.owned && fake.file_count == 0 && fake.record_count == 0);
    fake_event(FakeExit, 0);
    fake.exited = true;
    pthread_cond_broadcast(&fake.changed);
    pthread_mutex_unlock(&fake.lock);
    return NULL;
}
FuriThread* furi_thread_alloc_ex(
    const char* name,
    uint32_t stack,
    FuriThreadCallback callback,
    void* context) {
    assert(name && stack == 4096);
    FuriThread* thread = malloc(sizeof(*thread));
    assert(thread);
    *thread = (FuriThread){.callback = callback, .context = context};
    return thread;
}
void furi_thread_start(FuriThread* thread) {
    fake.worker_started = true;
    assert(pthread_create(&thread->id, NULL, fake_run, thread) == 0);
}
bool furi_thread_join(FuriThread* thread) {
    assert(!fake_worker && pthread_join(thread->id, NULL) == 0);
    fake.joined = true;
    fake_event(FakeJoin, 0);
    return true;
}
void furi_thread_free(FuriThread* thread) {
    assert(fake.joined);
    free(thread);
}
uint32_t furi_get_tick(void) {
    pthread_mutex_lock(&fake.lock);
    uint32_t tick = (uint32_t)fake.tick;
    pthread_mutex_unlock(&fake.lock);
    return tick;
}
uint32_t furi_kernel_get_tick_frequency(void) {
    return fake.hz;
}
uint32_t furi_ms_to_ticks(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * fake.hz + 999) / 1000);
}
int32_t furi_kernel_lock(void) {
    return 0;
}
int32_t furi_kernel_restore_lock(int32_t lock) {
    return lock;
}
size_t memmgr_heap_get_max_free_block(void) {
    if(fake.heap_fail_after == 0) return 0;
    if(fake.heap_fail_after > 0) fake.heap_fail_after--;
    return 1024 * 1024;
}
