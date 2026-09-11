#pragma once
#include "json_min.h"

#define FLOOPER_COUNTS_MAX         12
#define FLOOPER_ITEMS_MAX          8
#define FLOOPER_SLICES_MAX         8
#define FLOOPER_PATTERN_EVENTS_MAX 64
#define FLOOPER_EVENTS_MAX         128
#define FLOOPER_CANDIDATES_MAX     64
#define FLOOPER_INTERVALS_MAX      1024
#define FLOOPER_ID_BYTES           24
#define FLOOPER_NAME_BYTES         64

typedef enum {
    FlooperPatternOk,
    FlooperPatternJsonSyntax,
    FlooperPatternJsonSize,
    FlooperPatternJsonTokens,
    FlooperPatternJsonDepth,
    FlooperPatternJsonDuplicate,
    FlooperPatternNumber,
    FlooperPatternUnsupported,
    FlooperPatternMissing,
    FlooperPatternType,
    FlooperPatternInteger,
    FlooperPatternRange,
    FlooperPatternString,
    FlooperPatternLimit,
    FlooperPatternDuplicate,
    FlooperPatternReference,
    FlooperPatternDerivedTime,
    FlooperPatternContainment,
    FlooperPatternRenderPolicy
} FlooperPatternError;
typedef struct {
    uint32_t offset_us, duration_us;
    float frequency_hz, gain;
} FlooperSlice;
typedef struct {
    char id[FLOOPER_ID_BYTES];
    uint8_t priority, slice_count;
    FlooperSlice slices[FLOOPER_SLICES_MAX];
} FlooperVoice;
typedef struct {
    uint32_t offset_us;
    float velocity, source_intensity;
    uint8_t count_index, voice, layer;
    bool has_source_intensity;
} FlooperEvent;
typedef struct {
    char id[FLOOPER_ID_BYTES];
    uint16_t first_event, event_count;
} FlooperEventPattern;
typedef struct {
    char name[FLOOPER_ID_BYTES];
    uint16_t cycles;
    uint8_t pattern, layer_mask;
} FlooperSection;
typedef struct FlooperPattern {
    char name[FLOOPER_NAME_BYTES], display_name[FLOOPER_ID_BYTES];
    uint32_t pulse_us;
    uint64_t cycle_us;
    uint8_t count_count, labels[FLOOPER_COUNTS_MAX];
    uint8_t layer_count, voice_count, pattern_count, section_count, start_section;
    uint16_t event_count, accent_mask;
    bool repeat, warning_cycle_mismatch;
    uint32_t reported_cycle_us;
    float master_volume;
    char layers[FLOOPER_ITEMS_MAX][FLOOPER_ID_BYTES];
    FlooperVoice voices[FLOOPER_ITEMS_MAX];
    FlooperEventPattern patterns[FLOOPER_ITEMS_MAX];
    FlooperSection sections[FLOOPER_ITEMS_MAX];
    FlooperEvent events[FLOOPER_EVENTS_MAX];
} FlooperPattern;
typedef JsonMin FlooperPatternWorkspace;
/* Input/output/workspace must not alias. Caller owns bounded allocations;
 * output is completely zeroed on error, has no input pointers on success.
 * Workspace may be released immediately. No Storage, audio, logging or Furi. */
FlooperPatternError flooper_pattern_parse(
    const char* input,
    size_t length,
    FlooperPatternWorkspace* workspace,
    FlooperPattern* output);
const char* flooper_pattern_error_name(FlooperPatternError error);
