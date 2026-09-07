#define _XOPEN_SOURCE 700

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__arm__) || defined(__thumb__)

int main(void) {
    return 0;
}

#else

#define REQUIRE(condition)                 \
    do {                                   \
        if(!(condition)) __builtin_trap(); \
    } while(false)

typedef enum {
    BtStatusUnavailable,
    BtStatusOff,
    BtStatusAdvertising,
    BtStatusConnected,
} BtStatus;

typedef void (*BtStatusChangedCallback)(BtStatus status, void* context);

#define BT_STATUS_REGISTRATION_TYPES_DEFINED
#include "../../applications/services/bt/bt_service/bt_status_registration.h"

typedef struct {
    pthread_mutex_t dispatch;
    pthread_mutex_t status;
    pthread_mutex_t gate;
    pthread_cond_t changed;
    uint32_t dispatch_attempts;
    bool callback_entered;
    bool callback_may_return;
    bool callback_exited;
    bool unregister_returned;
    bool delivery_succeeded;
    bool unregister_succeeded;
    uint32_t callback_count;
    uint32_t context_magic;
    BtStatus current_status;
    BtStatus delivered_status;
    BtStatusChangedCallback callback;
    void* callback_context;
    BtStatusRegistration registration;
} LifetimeFixture;

enum {
    ContextMagicLive = 0x51A7E123U,
    ContextMagicPoisoned = 0xDEADF00DU,
};

static void recursive_mutex_init(pthread_mutex_t* mutex) {
    pthread_mutexattr_t attributes;
    REQUIRE(pthread_mutexattr_init(&attributes) == 0);
    REQUIRE(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) == 0);
    REQUIRE(pthread_mutex_init(mutex, &attributes) == 0);
    REQUIRE(pthread_mutexattr_destroy(&attributes) == 0);
}

static bool lifetime_dispatch_acquire(void* context, uint32_t timeout) {
    (void)timeout;
    LifetimeFixture* fixture = context;
    REQUIRE(pthread_mutex_lock(&fixture->gate) == 0);
    fixture->dispatch_attempts++;
    REQUIRE(pthread_cond_broadcast(&fixture->changed) == 0);
    REQUIRE(pthread_mutex_unlock(&fixture->gate) == 0);
    return pthread_mutex_lock(&fixture->dispatch) == 0;
}

static void lifetime_dispatch_release(void* context) {
    LifetimeFixture* fixture = context;
    REQUIRE(pthread_mutex_unlock(&fixture->dispatch) == 0);
}

static bool lifetime_status_acquire(void* context, uint32_t timeout) {
    (void)timeout;
    LifetimeFixture* fixture = context;
    return pthread_mutex_trylock(&fixture->status) == 0;
}

static void lifetime_status_release(void* context) {
    LifetimeFixture* fixture = context;
    REQUIRE(pthread_mutex_unlock(&fixture->status) == 0);
}

static BtStatus lifetime_status_snapshot(void* context) {
    return ((LifetimeFixture*)context)->current_status;
}

static void lifetime_callback(BtStatus status, void* context) {
    LifetimeFixture* fixture = context;
    REQUIRE(status == BtStatusConnected);
    REQUIRE(pthread_mutex_lock(&fixture->gate) == 0);
    fixture->callback_count++;
    fixture->callback_entered = true;
    REQUIRE(fixture->context_magic == ContextMagicLive);
    REQUIRE(pthread_cond_broadcast(&fixture->changed) == 0);
    while(!fixture->callback_may_return) {
        REQUIRE(pthread_cond_wait(&fixture->changed, &fixture->gate) == 0);
    }
    REQUIRE(fixture->context_magic == ContextMagicLive);
    fixture->callback_exited = true;
    REQUIRE(pthread_mutex_unlock(&fixture->gate) == 0);
}

static void* lifetime_delivery_thread(void* context) {
    LifetimeFixture* fixture = context;
    fixture->delivery_succeeded =
        bt_status_callback_deliver_bounded(&fixture->registration, BtStatusConnected, 100U);
    return NULL;
}

static void* lifetime_unregister_thread(void* context) {
    LifetimeFixture* fixture = context;
    bool succeeded = bt_status_callback_set_bounded(&fixture->registration, NULL, NULL, 100U);
    REQUIRE(pthread_mutex_lock(&fixture->gate) == 0);
    fixture->unregister_succeeded = succeeded;
    fixture->unregister_returned = true;
    if(succeeded) fixture->context_magic = ContextMagicPoisoned;
    REQUIRE(pthread_cond_broadcast(&fixture->changed) == 0);
    REQUIRE(pthread_mutex_unlock(&fixture->gate) == 0);
    return NULL;
}

static BtStatusRegistration lifetime_registration(LifetimeFixture* fixture) {
    return (BtStatusRegistration){
        .context = fixture,
        .acquire_dispatch = lifetime_dispatch_acquire,
        .release_dispatch = lifetime_dispatch_release,
        .acquire_status = lifetime_status_acquire,
        .release_status = lifetime_status_release,
        .snapshot = lifetime_status_snapshot,
        .callback_slot = &fixture->callback,
        .callback_context_slot = &fixture->callback_context,
        .delivered_status_slot = &fixture->delivered_status,
    };
}

static bool unregister_drains_snapshotted_callback_before_owner_poison(void) {
    LifetimeFixture fixture = {
        .context_magic = ContextMagicLive,
        .current_status = BtStatusConnected,
        .callback = lifetime_callback,
    };
    recursive_mutex_init(&fixture.dispatch);
    REQUIRE(pthread_mutex_init(&fixture.status, NULL) == 0);
    REQUIRE(pthread_mutex_init(&fixture.gate, NULL) == 0);
    REQUIRE(pthread_cond_init(&fixture.changed, NULL) == 0);
    fixture.callback_context = &fixture;
    fixture.registration = lifetime_registration(&fixture);

    pthread_t delivery_thread;
    pthread_t unregister_thread;
    REQUIRE(pthread_create(&delivery_thread, NULL, lifetime_delivery_thread, &fixture) == 0);

    REQUIRE(pthread_mutex_lock(&fixture.gate) == 0);
    while(!fixture.callback_entered) {
        REQUIRE(pthread_cond_wait(&fixture.changed, &fixture.gate) == 0);
    }
    REQUIRE(pthread_mutex_unlock(&fixture.gate) == 0);
    REQUIRE(pthread_create(&unregister_thread, NULL, lifetime_unregister_thread, &fixture) == 0);

    REQUIRE(pthread_mutex_lock(&fixture.gate) == 0);
    while(fixture.dispatch_attempts < 2U) {
        REQUIRE(pthread_cond_wait(&fixture.changed, &fixture.gate) == 0);
    }
    REQUIRE(!fixture.unregister_returned);
    REQUIRE(fixture.context_magic == ContextMagicLive);
    fixture.callback_may_return = true;
    REQUIRE(pthread_cond_broadcast(&fixture.changed) == 0);
    REQUIRE(pthread_mutex_unlock(&fixture.gate) == 0);

    REQUIRE(pthread_join(delivery_thread, NULL) == 0);
    REQUIRE(pthread_join(unregister_thread, NULL) == 0);
    REQUIRE(fixture.delivery_succeeded);
    REQUIRE(fixture.unregister_succeeded);
    REQUIRE(fixture.callback_exited);
    REQUIRE(fixture.callback_count == 1U);
    REQUIRE(fixture.context_magic == ContextMagicPoisoned);
    REQUIRE(fixture.callback == NULL);
    REQUIRE(fixture.callback_context == NULL);
    REQUIRE(bt_status_callback_deliver_bounded(&fixture.registration, BtStatusAdvertising, 100U));
    REQUIRE(fixture.callback_count == 1U);
    return true;
}

typedef struct {
    LifetimeFixture base;
    bool inject_requested;
    bool arm_injection;
    BtStatus observed[2];
    uint32_t observed_count;
} OrderingFixture;

static void ordering_status_release(void* context) {
    OrderingFixture* fixture = context;
    REQUIRE(pthread_mutex_unlock(&fixture->base.status) == 0);
    if(!fixture->arm_injection) return;
    fixture->arm_injection = false;
    REQUIRE(pthread_mutex_lock(&fixture->base.gate) == 0);
    fixture->inject_requested = true;
    REQUIRE(pthread_cond_broadcast(&fixture->base.changed) == 0);
    while(fixture->base.dispatch_attempts < 2U) {
        REQUIRE(pthread_cond_wait(&fixture->base.changed, &fixture->base.gate) == 0);
    }
    REQUIRE(pthread_mutex_unlock(&fixture->base.gate) == 0);
}

static void ordering_callback(BtStatus status, void* context) {
    OrderingFixture* fixture = context;
    REQUIRE(pthread_mutex_lock(&fixture->base.gate) == 0);
    REQUIRE(fixture->observed_count < 2U);
    fixture->observed[fixture->observed_count++] = status;
    REQUIRE(pthread_mutex_unlock(&fixture->base.gate) == 0);
}

static void* ordering_injection_thread(void* context) {
    OrderingFixture* fixture = context;
    REQUIRE(pthread_mutex_lock(&fixture->base.gate) == 0);
    while(!fixture->inject_requested) {
        REQUIRE(pthread_cond_wait(&fixture->base.changed, &fixture->base.gate) == 0);
    }
    fixture->base.current_status = BtStatusConnected;
    REQUIRE(pthread_mutex_unlock(&fixture->base.gate) == 0);
    REQUIRE(
        bt_status_callback_deliver_bounded(&fixture->base.registration, BtStatusConnected, 100U));
    return NULL;
}

static bool initial_snapshot_precedes_newer_delivery(void) {
    OrderingFixture fixture = {
        .base.current_status = BtStatusOff,
        .arm_injection = true,
    };
    recursive_mutex_init(&fixture.base.dispatch);
    REQUIRE(pthread_mutex_init(&fixture.base.status, NULL) == 0);
    REQUIRE(pthread_mutex_init(&fixture.base.gate, NULL) == 0);
    REQUIRE(pthread_cond_init(&fixture.base.changed, NULL) == 0);
    fixture.base.registration = lifetime_registration(&fixture.base);
    fixture.base.registration.release_status = ordering_status_release;

    pthread_t injection_thread;
    REQUIRE(pthread_create(&injection_thread, NULL, ordering_injection_thread, &fixture) == 0);
    BtStatus initial_status;
    REQUIRE(bt_status_register_and_deliver_ordered(
        &fixture.base.registration, ordering_callback, &fixture, 100U, &initial_status));
    REQUIRE(pthread_join(injection_thread, NULL) == 0);
    REQUIRE(initial_status == BtStatusOff);
    REQUIRE(fixture.observed_count == 2U);
    REQUIRE(fixture.observed[0] == BtStatusOff);
    REQUIRE(fixture.observed[1] == BtStatusConnected);
    return true;
}

static void self_unregister_callback(BtStatus status, void* context) {
    LifetimeFixture* fixture = context;
    REQUIRE(status == BtStatusAdvertising);
    fixture->callback_count++;
    REQUIRE(bt_status_callback_set_bounded(&fixture->registration, NULL, NULL, 100U));
}

static bool callback_self_unregisters_without_deadlock_or_later_delivery(void) {
    LifetimeFixture fixture = {.current_status = BtStatusAdvertising};
    recursive_mutex_init(&fixture.dispatch);
    REQUIRE(pthread_mutex_init(&fixture.status, NULL) == 0);
    REQUIRE(pthread_mutex_init(&fixture.gate, NULL) == 0);
    REQUIRE(pthread_cond_init(&fixture.changed, NULL) == 0);
    fixture.registration = lifetime_registration(&fixture);

    BtStatus initial_status;
    REQUIRE(bt_status_register_and_deliver_ordered(
        &fixture.registration, self_unregister_callback, &fixture, 100U, &initial_status));
    REQUIRE(initial_status == BtStatusAdvertising);
    REQUIRE(fixture.callback_count == 1U);
    REQUIRE(fixture.callback == NULL);
    REQUIRE(fixture.callback_context == NULL);
    REQUIRE(bt_status_callback_deliver_bounded(&fixture.registration, BtStatusConnected, 100U));
    REQUIRE(fixture.callback_count == 1U);
    return true;
}

static bool unavailable_dispatch(void* context, uint32_t timeout) {
    (void)context;
    (void)timeout;
    return false;
}

static bool unregister_timeout_preserves_registration(void) {
    BtStatusChangedCallback callback = lifetime_callback;
    void* callback_context = &callback;
    const BtStatusRegistration registration = {
        .acquire_dispatch = unavailable_dispatch,
        .callback_slot = &callback,
        .callback_context_slot = &callback_context,
    };

    REQUIRE(!bt_status_callback_set_bounded(&registration, NULL, NULL, 100U));
    REQUIRE(callback == lifetime_callback);
    REQUIRE(callback_context == &callback);
    return true;
}

int main(void) {
    if(!unregister_drains_snapshotted_callback_before_owner_poison()) return 1;
    if(!initial_snapshot_precedes_newer_delivery()) return 1;
    if(!callback_self_unregisters_without_deadlock_or_later_delivery()) return 1;
    if(!unregister_timeout_preserves_registration()) return 1;
    return 0;
}

#endif
