#pragma once

#include "flooper_pattern.h"

#define FLOOPER_RANGES_MAX (FLOOPER_ITEMS_MAX * FLOOPER_COUNTS_MAX)

typedef enum {
    FlooperScheduleOk,
    FlooperScheduleInvalid,
    FlooperScheduleReference,
    FlooperScheduleContainment,
    FlooperScheduleOverflow,
    FlooperScheduleCandidateLimit,
    FlooperScheduleIntervalLimit,
    FlooperSchedulePositionEnd
} FlooperScheduleError;
typedef struct {
    uint32_t start_us, end_us;
    float frequency_hz, volume;
} FlooperInterval;
typedef struct {
    uint16_t first_interval, interval_count;
    uint8_t pattern, layer_mask, count_index;
} FlooperCountRange;
typedef struct {
    uint64_t start_us, duration_us, first_count;
    uint16_t cycles, first_range;
} FlooperScheduleSection;
typedef struct FlooperSchedule {
    uint64_t cycle_us, form_us, total_counts;
    uint32_t pulse_us;
    uint16_t interval_count, range_count;
    uint8_t count_count, section_count, start_section;
    bool repeat;
    uint8_t labels[FLOOPER_COUNTS_MAX];
    FlooperScheduleSection sections[FLOOPER_ITEMS_MAX];
    FlooperCountRange ranges[FLOOPER_RANGES_MAX];
    FlooperInterval intervals[FLOOPER_INTERVALS_MAX];
} FlooperSchedule;
typedef struct {
    uint64_t elapsed_count;
    uint64_t form_index, count_start_us;
    uint16_t cycle_index, range_index;
    uint8_t section_index, count_index, label;
} FlooperPosition;

/* Loading only: disjoint caller-owned model/output; no allocation or side effects.
 * Output must be off-stack and unpublished during compilation. Errors zero it.
 * Publish only on Ok, then treat as const for its entire lifetime. No input
 * pointers survive; repeated sections share ranges, never expanded cycles.
 * Intervals are half-open COUNT-LOCAL coordinates, silence is an absent span.
 * Coalescing removes only inaudible interior boundaries, never outer endpoints. */
FlooperScheduleError
    flooper_schedule_compile(const FlooperPattern* model, FlooperSchedule* output);

/* For a successful immutable schedule: elapsed_count is from form section zero,
 * including repeated forms. start_section is metadata for the later player.
 * Caller supplies elapsed_count; success fills the other fields, error clears
 * them (preserving elapsed_count). Add count_start_us to local boundaries ONCE.
 * Checks the entire count end, so any contained local endpoint also fits u64. */
FlooperScheduleError
    flooper_schedule_position(const FlooperSchedule* schedule, FlooperPosition* position);
