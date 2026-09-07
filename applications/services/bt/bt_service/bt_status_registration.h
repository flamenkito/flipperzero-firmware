#pragma once

#ifndef BT_STATUS_REGISTRATION_TYPES_DEFINED
#include "bt.h"
#endif

typedef bool (*BtStatusCallbackAcquire)(void* context, uint32_t timeout);
typedef void (*BtStatusCallbackRelease)(void* context);
typedef BtStatus (*BtStatusRegistrationSnapshot)(void* context);

typedef struct {
    void* context;
    BtStatusCallbackAcquire acquire_dispatch;
    BtStatusCallbackRelease release_dispatch;
    BtStatusCallbackAcquire acquire_status;
    BtStatusCallbackRelease release_status;
    BtStatusRegistrationSnapshot snapshot;
    BtStatusChangedCallback* callback_slot;
    void** callback_context_slot;
    BtStatus* delivered_status_slot;
} BtStatusRegistration;

/* Lock order is always dispatch -> status. Application callbacks run with the
 * recursive dispatch lock held and the status lock released. Consequently an
 * external unregister drains an in-flight callback before returning, while a
 * callback may recursively unregister itself without deadlocking. */
static inline bool bt_status_callback_set_bounded(
    const BtStatusRegistration* registration,
    BtStatusChangedCallback callback,
    void* callback_context,
    uint32_t timeout) {
    if(!registration->acquire_dispatch(registration->context, timeout)) return false;
    if(!registration->acquire_status(registration->context, 0U)) {
        registration->release_dispatch(registration->context);
        return false;
    }
    *registration->callback_context_slot = callback_context;
    *registration->callback_slot = callback;
    registration->release_status(registration->context);
    registration->release_dispatch(registration->context);
    return true;
}

static inline bool bt_status_callback_deliver_bounded(
    const BtStatusRegistration* registration,
    BtStatus status,
    uint32_t timeout) {
    if(!registration->acquire_dispatch(registration->context, timeout)) return false;
    if(!registration->acquire_status(registration->context, 0U)) {
        registration->release_dispatch(registration->context);
        return false;
    }
    if(registration->delivered_status_slot) *registration->delivered_status_slot = status;
    BtStatusChangedCallback callback = *registration->callback_slot;
    void* callback_context = *registration->callback_context_slot;
    registration->release_status(registration->context);
    if(callback) callback(status, callback_context);
    registration->release_dispatch(registration->context);
    return true;
}

static inline bool bt_status_register_and_deliver_ordered(
    const BtStatusRegistration* registration,
    BtStatusChangedCallback callback,
    void* callback_context,
    uint32_t timeout,
    BtStatus* delivered_status) {
    if(!registration->acquire_dispatch(registration->context, timeout)) return false;
    if(!registration->acquire_status(registration->context, 0U)) {
        registration->release_dispatch(registration->context);
        return false;
    }
    *registration->callback_context_slot = callback_context;
    *registration->callback_slot = callback;
    BtStatus status = registration->snapshot(registration->context);
    registration->release_status(registration->context);
    *delivered_status = status;
    if(callback) callback(status, callback_context);
    registration->release_dispatch(registration->context);
    return true;
}
