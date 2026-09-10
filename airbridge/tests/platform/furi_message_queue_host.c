#include "furi.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

extern int pthread_cond_timedwait(
    pthread_cond_t* condition,
    pthread_mutex_t* mutex,
    const struct timespec* absolute_time);

struct FuriMessageQueue {
    pthread_mutex_t mutex;
    pthread_cond_t readable;
    pthread_cond_t writable;
    uint8_t* storage;
    size_t capacity;
    size_t message_size;
    size_t head;
    size_t count;
};

static int queue_wait(pthread_cond_t* condition, pthread_mutex_t* mutex, uint32_t timeout) {
    if(timeout == 0) return ETIMEDOUT;
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += timeout / 1000U;
    deadline.tv_nsec += (long)(timeout % 1000U) * 1000000L;
    if(deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(condition, mutex, &deadline);
}

FuriMessageQueue* furi_message_queue_alloc(size_t message_count, size_t message_size) {
    assert(message_count > 0 && message_size > 0);
    FuriMessageQueue* queue = calloc(1, sizeof(*queue));
    assert(queue != NULL);
    queue->storage = calloc(message_count, message_size);
    assert(queue->storage != NULL);
    queue->capacity = message_count;
    queue->message_size = message_size;
    assert(pthread_mutex_init(&queue->mutex, NULL) == 0);
    assert(pthread_cond_init(&queue->readable, NULL) == 0);
    assert(pthread_cond_init(&queue->writable, NULL) == 0);
    return queue;
}

void furi_message_queue_free(FuriMessageQueue* queue) {
    if(queue == NULL) return;
    assert(pthread_cond_destroy(&queue->readable) == 0);
    assert(pthread_cond_destroy(&queue->writable) == 0);
    assert(pthread_mutex_destroy(&queue->mutex) == 0);
    free(queue->storage);
    free(queue);
}

FuriStatus furi_message_queue_put(FuriMessageQueue* queue, const void* message, uint32_t timeout) {
    assert(queue != NULL && message != NULL);
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    while(queue->count == queue->capacity) {
        if(queue_wait(&queue->writable, &queue->mutex, timeout) != 0) {
            assert(pthread_mutex_unlock(&queue->mutex) == 0);
            return FuriStatusErrorTimeout;
        }
    }
    const size_t tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->storage + tail * queue->message_size, message, queue->message_size);
    queue->count++;
    assert(pthread_cond_signal(&queue->readable) == 0);
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return FuriStatusOk;
}

FuriStatus furi_message_queue_get(FuriMessageQueue* queue, void* message, uint32_t timeout) {
    assert(queue != NULL && message != NULL);
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    while(queue->count == 0) {
        if(queue_wait(&queue->readable, &queue->mutex, timeout) != 0) {
            assert(pthread_mutex_unlock(&queue->mutex) == 0);
            return FuriStatusErrorTimeout;
        }
    }
    memcpy(message, queue->storage + queue->head * queue->message_size, queue->message_size);
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    assert(pthread_cond_signal(&queue->writable) == 0);
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return FuriStatusOk;
}
