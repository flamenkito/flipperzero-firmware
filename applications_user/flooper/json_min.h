#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLOOPER_JSON_BYTES  16384
#define FLOOPER_JSON_TOKENS 2048
#define FLOOPER_JSON_DEPTH  16

typedef enum {
    JsonMinOk,
    JsonMinSyntax,
    JsonMinSize,
    JsonMinTokens,
    JsonMinDepth,
    JsonMinDuplicate,
    JsonMinNumber
} JsonMinError;
typedef enum {
    JsonMissing,
    JsonObject,
    JsonArray,
    JsonString,
    JsonNumber,
    JsonBool,
    JsonNull
} JsonMinType;
typedef struct {
    uint16_t start, length, next;
    uint8_t type;
} JsonMinToken;
/* Caller-owned transient workspace. Token zero is the missing-value sentinel;
 * strings decode in-place; next skips an entire subtree. No retained heap AST. */
typedef struct {
    char text[FLOOPER_JSON_BYTES + 1];
    JsonMinToken tokens[FLOOPER_JSON_TOKENS + 1];
    uint16_t used, position, length;
    JsonMinError error;
} JsonMin;
JsonMinError json_min_parse(JsonMin* json, const char* text, size_t length);
bool json_min_string(JsonMin* json, uint16_t token);
bool json_min_equal(const JsonMin* json, uint16_t token, const char* text);
uint16_t json_min_get(const JsonMin* json, uint16_t object, const char* key);
bool json_min_uint(const JsonMin* json, uint16_t token, uint32_t* value);
