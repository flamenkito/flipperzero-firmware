#pragma once

#ifndef BT_STATUS_REGISTRATION_TYPES_DEFINED
#include "bt.h"
#endif

typedef void (*BtStatusRegistrationLock)(void* context);
typedef BtStatus (*BtStatusRegistrationSnapshot)(void* context);

typedef struct {
    void* context;
    BtStatusRegistrationLock lock;
    BtStatusRegistrationLock unlock;
    BtStatusRegistrationSnapshot snapshot;
    BtStatusChangedCallback* callback_slot;
    void** callback_context_slot;
} BtStatusRegistration;

static inline BtStatus bt_status_register_and_deliver_ordered(
    const BtStatusRegistration* registration,
    BtStatusChangedCallback callback,
    void* callback_context) {
    registration->lock(registration->context);
    *registration->callback_context_slot = callback_context;
    *registration->callback_slot = callback;
    BtStatus status = registration->snapshot(registration->context);
    if(callback) callback(status, callback_context);
    registration->unlock(registration->context);
    return status;
}
