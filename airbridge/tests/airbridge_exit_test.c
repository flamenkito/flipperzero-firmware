#include <assert.h>
#include <pthread.h>

#include "../../applications_user/pocket_airbridge/airbridge_lifecycle.h"

typedef enum {
    StageClosing,
    StageUsb,
    StageBle,
    StageCount
} Stage;

typedef struct {
    AirbridgeOperationMonitor operation;
    AirbridgeExitContract exit;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool entered[StageCount];
    bool may_return[StageCount];
    unsigned usb_attempts;
} Fixture;

static void wait_at_stage(Fixture* fixture, Stage stage) {
    pthread_mutex_lock(&fixture->mutex);
    fixture->entered[stage] = true;
    pthread_cond_broadcast(&fixture->changed);
    while(!fixture->may_return[stage])
        pthread_cond_wait(&fixture->changed, &fixture->mutex);
    pthread_mutex_unlock(&fixture->mutex);
}

static void show_closing(void* context) {
    wait_at_stage(context, StageClosing);
}

static bool restore_usb(void* context) {
    Fixture* fixture = context;
    wait_at_stage(fixture, StageUsb);
    /* An unsuccessful restore must be retried, not treated as detached. */
    return ++fixture->usb_attempts > 1;
}

static bool restore_ble(void* context) {
    wait_at_stage(context, StageBle);
    return true;
}

static uint32_t now(void* context) {
    (void)context;
    return 1000;
}

static void retry_wait(void* context) {
    Fixture* fixture = context;
    assert(!airbridge_exit_contract_may_destroy(&fixture->exit));
}

static void* cleanup_worker(void* context) {
    Fixture* fixture = context;
    const AirbridgeLifecycleOps ops = {
        .context = fixture,
        .show_closing = show_closing,
        .restore_usb = restore_usb,
        .restore_ble = restore_ble,
        .now = now,
        .wait = retry_wait,
    };
    airbridge_lifecycle_close(&ops, &fixture->operation, &fixture->exit);
    return NULL;
}

static void await_stage(Fixture* fixture, Stage stage) {
    while(!fixture->entered[stage])
        pthread_cond_wait(&fixture->changed, &fixture->mutex);
}

static void release_stage(Fixture* fixture, Stage stage) {
    fixture->may_return[stage] = true;
    pthread_cond_broadcast(&fixture->changed);
}

int main(void) {
    Fixture fixture = {
        .exit = airbridge_exit_contract_initial(),
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    pthread_t worker;
    assert(pthread_create(&worker, NULL, cleanup_worker, &fixture) == 0);
    pthread_mutex_lock(&fixture.mutex);

    await_stage(&fixture, StageClosing);
    assert(!fixture.entered[StageUsb] && !fixture.entered[StageBle]);
    assert(!airbridge_exit_contract_may_destroy(&fixture.exit));
    release_stage(&fixture, StageClosing);
    await_stage(&fixture, StageUsb);
    assert(!fixture.entered[StageBle]);
    assert(!airbridge_exit_contract_may_destroy(&fixture.exit));
    release_stage(&fixture, StageUsb);
    await_stage(&fixture, StageBle);
    assert(fixture.usb_attempts == 2);

    /* The production cleanup sequence remains blocked on Bluetooth while its
     * independent supervisor detects the deadline and retains the owner. */
    assert(airbridge_operation_observe(
               &fixture.operation, 1000 + AirbridgeOperationBleBudgetMs, false)
               .stalled);
    assert(!airbridge_exit_contract_may_destroy(&fixture.exit));
    release_stage(&fixture, StageBle);
    pthread_mutex_unlock(&fixture.mutex);
    assert(pthread_join(worker, NULL) == 0);
    assert(airbridge_exit_contract_may_destroy(&fixture.exit));
    assert(airbridge_operation_observe(&fixture.operation, 22000, false).stalled);
    pthread_cond_destroy(&fixture.changed);
    pthread_mutex_destroy(&fixture.mutex);
    return 0;
}
