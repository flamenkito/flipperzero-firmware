#pragma once
#include "flooper_pattern.h"

typedef struct {
    JsonMin* json;
    FlooperPattern* out;
    FlooperPatternError error;
} FlooperReader;
void fp_fail(FlooperReader* r, FlooperPatternError error);
bool fp_type(FlooperReader* r, uint16_t token, JsonMinType type);
uint16_t fp_get(FlooperReader* r, uint16_t object, const char* key);
uint16_t fp_array(FlooperReader* r, uint16_t token, unsigned maximum);
uint16_t fp_at(FlooperReader* r, uint16_t array, unsigned index);
uint32_t fp_uint(FlooperReader* r, uint16_t token, uint32_t minimum, uint32_t maximum);
double fp_real(FlooperReader* r, uint16_t token, double minimum, double maximum);
bool fp_bool(FlooperReader* r, uint16_t token);
void fp_string(FlooperReader* r, uint16_t token, char* out, size_t capacity);
uint8_t fp_label(FlooperReader* r, uint16_t token);
uint8_t fp_reference(FlooperReader* r, uint16_t token, unsigned kind);
void fp_timebase(FlooperReader* r, bool draft);
void fp_voices(FlooperReader* r);
void fp_events(FlooperReader* r, uint16_t array, unsigned pattern, bool draft);
void fp_sections(FlooperReader* r, bool draft);
void fp_finish(FlooperReader* r);
void fp_canonical(FlooperReader* r);
