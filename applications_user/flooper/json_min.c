#include "json_min.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void space(JsonMin* j) {
    while(j->position < j->length && strchr(" \t\r\n", j->text[j->position]))
        ++j->position;
}

static bool digit(char c) {
    return c >= '0' && c <= '9';
}

static bool number(JsonMin* j) {
    size_t start = j->position;
    if(j->text[j->position] == '-') ++j->position;
    if(j->text[j->position] == '0')
        ++j->position;
    else {
        if(!digit(j->text[j->position])) return false;
        while(digit(j->text[j->position]))
            ++j->position;
    }
    if(j->text[j->position] == '.') {
        ++j->position;
        if(!digit(j->text[j->position])) return false;
        while(digit(j->text[j->position]))
            ++j->position;
    }
    if(j->text[j->position] == 'e' || j->text[j->position] == 'E') {
        ++j->position;
        if(j->text[j->position] == '+' || j->text[j->position] == '-') ++j->position;
        if(!digit(j->text[j->position])) return false;
        while(digit(j->text[j->position]))
            ++j->position;
    }
    errno = 0;
    double value = strtod(j->text + start, NULL);
    if(errno == ERANGE || !isfinite(value)) j->error = JsonMinNumber;
    return true;
}

static uint16_t value(JsonMin* j, unsigned depth);

static bool container(JsonMin* j, uint16_t token, unsigned depth) {
    bool object = j->tokens[token].type == JsonObject;
    char close = object ? '}' : ']';
    ++j->position;
    space(j);
    if(j->text[j->position] == close) {
        ++j->position;
        return true;
    }
    while(j->error == JsonMinOk) {
        uint16_t key = value(j, depth);
        if(!key) return false;
        if(object) {
            if(j->tokens[key].type != JsonString) return false;
            for(uint16_t k = token + 1; k < key; k = j->tokens[k + 1].next) {
                if(j->tokens[k].length == j->tokens[key].length &&
                   memcmp(
                       j->text + j->tokens[k].start,
                       j->text + j->tokens[key].start,
                       j->tokens[key].length) == 0) {
                    j->error = JsonMinDuplicate;
                    return false;
                }
            }
            space(j);
            if(j->text[j->position++] != ':') return false;
            if(!value(j, depth)) return false;
        }
        space(j);
        if(j->text[j->position] == close) {
            ++j->position;
            return true;
        }
        if(j->text[j->position++] != ',') return false;
    }
    return false;
}

static uint16_t value(JsonMin* j, unsigned depth) {
    space(j);
    if(j->position >= j->length) return 0;
    if(j->used == FLOOPER_JSON_TOKENS) {
        j->error = JsonMinTokens;
        return 0;
    }
    uint16_t id = ++j->used;
    JsonMinToken* t = &j->tokens[id];
    t->start = j->position;
    bool ok = false;
    switch(j->text[j->position]) {
    case '{':
    case '[':
        if(depth == FLOOPER_JSON_DEPTH) {
            j->error = JsonMinDepth;
            return 0;
        }
        t->type = j->text[j->position] == '{' ? JsonObject : JsonArray;
        ok = container(j, id, depth + 1);
        break;
    case '"':
        t->type = JsonString;
        ok = json_min_string(j, id);
        break;
    case 't':
    case 'f':
    case 'n': {
        const char* literal = j->text[j->position] == 't' ? "true" :
                              j->text[j->position] == 'f' ? "false" :
                                                            "null";
        size_t n = strlen(literal);
        ok = n <= (size_t)(j->length - j->position) &&
             memcmp(j->text + j->position, literal, n) == 0;
        t->type = literal[0] == 'n' ? JsonNull : JsonBool;
        if(ok) j->position += n;
        break;
    }
    default:
        t->type = JsonNumber;
        ok = number(j);
        break;
    }
    if(!ok || j->error != JsonMinOk) return 0;
    if(t->type != JsonString) t->length = j->position - t->start;
    t->next = j->used + 1;
    return id;
}

JsonMinError json_min_parse(JsonMin* j, const char* text, size_t length) {
    memset(j, 0, sizeof(*j));
    if(length > FLOOPER_JSON_BYTES) return j->error = JsonMinSize;
    if(!text || memchr(text, 0, length)) return j->error = JsonMinSyntax;
    memcpy(j->text, text, length);
    j->length = length;
    uint16_t root = value(j, 0);
    space(j);
    if(j->error == JsonMinOk && (!root || j->position != length)) j->error = JsonMinSyntax;
    return j->error;
}

bool json_min_equal(const JsonMin* j, uint16_t token, const char* text) {
    const JsonMinToken* t = &j->tokens[token];
    return t->type == JsonString && t->length == strlen(text) &&
           memcmp(j->text + t->start, text, t->length) == 0;
}

uint16_t json_min_get(const JsonMin* j, uint16_t object, const char* key) {
    if(j->tokens[object].type != JsonObject) return 0;
    for(uint16_t i = object + 1; i < j->tokens[object].next; i = j->tokens[i + 1].next)
        if(json_min_equal(j, i, key)) return i + 1;
    return 0;
}

/* Decimal exactness is checked before conversion, not against rounded double.
 * Remove exponent/fraction scale only when all discarded digits are zero. */
bool json_min_uint(const JsonMin* j, uint16_t token, uint32_t* result) {
    const JsonMinToken* t = &j->tokens[token];
    if(t->type != JsonNumber) return false;
    const char* s = j->text + t->start;
    size_t n = t->length, end = 0, fraction = 0;
    bool dot = false, negative = s[0] == '-';
    size_t begin = negative ? 1 : 0;
    for(end = begin; end < n && s[end] != 'e' && s[end] != 'E'; ++end) {
        if(s[end] == '.')
            dot = true;
        else if(dot)
            ++fraction;
    }
    int exponent = 0;
    if(end < n) {
        size_t p = end + 1;
        bool minus = s[p] == '-';
        if(s[p] == '-' || s[p] == '+') ++p;
        for(; p < n; ++p) {
            if(exponent > 32768) return false;
            exponent = exponent * 10 + s[p] - '0';
        }
        if(minus) exponent = -exponent;
    }
    int scale = exponent - (int)fraction;
    while(scale < 0 && end > begin) {
        char c = s[--end];
        if(c == '.') continue;
        if(c != '0') return false;
        ++scale;
    }
    uint32_t number_value = 0;
    for(size_t p = begin; p < end; ++p) {
        if(s[p] == '.') continue;
        unsigned d = s[p] - '0';
        if(number_value > (UINT32_MAX - d) / 10) return false;
        number_value = number_value * 10 + d;
    }
    if(number_value && negative) return false;
    if(number_value && scale > 10) return false;
    while(number_value && scale-- > 0) {
        if(number_value > UINT32_MAX / 10) return false;
        number_value *= 10;
    }
    *result = number_value;
    return true;
}
