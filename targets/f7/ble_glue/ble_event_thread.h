#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controls for thread handling SHCI & HCI event queues. Used internally. */

void ble_event_thread_start(void);

void ble_event_thread_stop(void);

bool ble_event_thread_stop_bounded(uint32_t timeout);

bool ble_event_thread_quiesce_bounded(uint32_t timeout);
void ble_event_thread_free_stopped(void);

#ifdef __cplusplus
}
#endif
