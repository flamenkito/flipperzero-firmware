#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../applications_user/pocket_airbridge/airbridge_exit_contract.h"

#define REQUIRE(condition) \
    do {                   \
        if(!(condition)) __builtin_trap(); \
    } while(false)

#define TEST_SIGNAL_EXIT 1U

static AirbridgeExitLatch exit_latch = AIRBRIDGE_EXIT_LATCH_INITIALIZER;

typedef bool (*TestSignalCallback)(uint32_t signal, void* arg, void* context);

typedef struct {
    TestSignalCallback callback;
    void* callback_context;
} SignalFixture;

typedef struct {
    uint32_t poison_a;
    uint32_t poison_b;
} PoisonedAppState;

static bool test_signal_callback(uint32_t signal, void* arg, void* context) {
    (void)arg;
    (void)context;
    if(signal != TEST_SIGNAL_EXIT) return false;
    airbridge_exit_latch_request(&exit_latch);
    return true;
}

static bool late_signals_never_touch_poisoned_app_state(void) {
    PoisonedAppState app_state = {
        .poison_a = UINT32_C(0xA5A5A5A5),
        .poison_b = UINT32_C(0x5A5A5A5A),
    };
    SignalFixture fixture = {.callback_context = &app_state};
    airbridge_exit_latch_reset(&exit_latch);

    fixture.callback = test_signal_callback;
    TestSignalCallback in_flight_callback = fixture.callback;
    void* stale_context = fixture.callback_context;
    fixture.callback = NULL;
    fixture.callback_context = NULL;

    for(uint32_t i = 0; i < 10000U; i++) {
        REQUIRE(in_flight_callback(TEST_SIGNAL_EXIT, NULL, stale_context));
    }
    REQUIRE(airbridge_exit_latch_requested(&exit_latch));
    REQUIRE(app_state.poison_a == UINT32_C(0xA5A5A5A5));
    REQUIRE(app_state.poison_b == UINT32_C(0x5A5A5A5A));
    REQUIRE(in_flight_callback(TEST_SIGNAL_EXIT, NULL, (void*)(uintptr_t)1));
    return true;
}

static bool exit_is_observed_within_poll_budget(void) {
    airbridge_exit_latch_reset(&exit_latch);
    REQUIRE(!airbridge_exit_latch_requested(&exit_latch));
    REQUIRE(test_signal_callback(TEST_SIGNAL_EXIT, NULL, (void*)(uintptr_t)1));

    uint32_t elapsed_ms = 0;
    while(!airbridge_exit_latch_requested(&exit_latch) && elapsed_ms <= 100U) {
        elapsed_ms += AIRBRIDGE_EXIT_POLL_INTERVAL_MS;
    }
    REQUIRE(airbridge_exit_latch_requested(&exit_latch));
    REQUIRE(AIRBRIDGE_EXIT_POLL_INTERVAL_MS <= 100U);
    REQUIRE(elapsed_ms <= 100U);
    return true;
}

int main(void) {
    if(!late_signals_never_touch_poisoned_app_state()) return 1;
    if(!exit_is_observed_within_poll_budget()) return 1;
    return 0;
}
