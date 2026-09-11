#include "flooper_pattern_internal.h"
#include <stdlib.h>
#include <string.h>

void fp_fail(FlooperReader* r, FlooperPatternError error) {
    if(r->error == FlooperPatternOk) r->error = error;
}
bool fp_type(FlooperReader* r, uint16_t token, JsonMinType type) {
    if(r->error != FlooperPatternOk) return false;
    if(!token)
        fp_fail(r, FlooperPatternMissing);
    else if(r->json->tokens[token].type != type)
        fp_fail(r, FlooperPatternType);
    return r->error == FlooperPatternOk;
}
uint16_t fp_get(FlooperReader* r, uint16_t object, const char* key) {
    if(!fp_type(r, object, JsonObject)) return 0;
    return json_min_get(r->json, object, key);
}
uint16_t fp_array(FlooperReader* r, uint16_t token, unsigned maximum) {
    if(!fp_type(r, token, JsonArray)) return 0;
    unsigned count = 0;
    for(uint16_t i = token + 1; i < r->json->tokens[token].next; i = r->json->tokens[i].next)
        ++count;
    if(count > maximum) {
        fp_fail(r, FlooperPatternLimit);
        return 0;
    }
    return count;
}
uint16_t fp_at(FlooperReader* r, uint16_t array, unsigned index) {
    if(!fp_type(r, array, JsonArray)) return 0;
    uint16_t i = array + 1;
    while(index-- && i < r->json->tokens[array].next)
        i = r->json->tokens[i].next;
    return i < r->json->tokens[array].next ? i : 0;
}
uint32_t fp_uint(FlooperReader* r, uint16_t token, uint32_t minimum, uint32_t maximum) {
    if(!fp_type(r, token, JsonNumber)) return minimum;
    uint32_t value;
    if(!json_min_uint(r->json, token, &value)) {
        fp_fail(r, FlooperPatternInteger);
        return minimum;
    }
    if(value < minimum || value > maximum) {
        fp_fail(r, FlooperPatternRange);
        return minimum;
    }
    return value;
}
double fp_real(FlooperReader* r, uint16_t token, double minimum, double maximum) {
    if(!fp_type(r, token, JsonNumber)) return minimum;
    double value = strtod(r->json->text + r->json->tokens[token].start, NULL);
    if(value < minimum || value > maximum) {
        fp_fail(r, FlooperPatternRange);
        return minimum;
    }
    return value;
}
bool fp_bool(FlooperReader* r, uint16_t token) {
    return fp_type(r, token, JsonBool) && r->json->text[r->json->tokens[token].start] == 't';
}
void fp_string(FlooperReader* r, uint16_t token, char* out, size_t capacity) {
    if(!fp_type(r, token, JsonString)) return;
    JsonMinToken t = r->json->tokens[token];
    if(!t.length || t.length >= capacity) {
        fp_fail(r, FlooperPatternString);
        return;
    }
    for(unsigned i = 0; i < t.length; ++i) {
        unsigned char c = r->json->text[t.start + i];
        if(c < 0x20 || c > 0x7e) {
            fp_fail(r, FlooperPatternString);
            return;
        }
    }
    memcpy(out, r->json->text + t.start, t.length);
    out[t.length] = 0;
}
uint8_t fp_label(FlooperReader* r, uint16_t token) {
    uint32_t label = fp_uint(r, token, 1, 99);
    for(unsigned i = 0; i < r->out->count_count; ++i)
        if(r->out->labels[i] == label) return i;
    fp_fail(r, FlooperPatternReference);
    return 0;
}
uint8_t fp_reference(FlooperReader* r, uint16_t token, unsigned kind) {
    char id[FLOOPER_ID_BYTES] = {0};
    fp_string(r, token, id, sizeof(id));
    FlooperPattern* d = r->out;
    unsigned count = kind == 0 ? d->layer_count : kind == 1 ? d->voice_count : d->pattern_count;
    for(unsigned i = 0; i < count; ++i) {
        const char* name = kind == 0 ? d->layers[i] :
                           kind == 1 ? d->voices[i].id :
                                       d->patterns[i].id;
        if(strcmp(id, name) == 0) return i;
    }
    fp_fail(r, FlooperPatternReference);
    return 0;
}
