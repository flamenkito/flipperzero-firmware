#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRBRIDGE_PASSWORD_COUNT    32U
#define AIRBRIDGE_PASSWORD_NAME     24U
#define AIRBRIDGE_PASSWORD_SIZE     128U
#define AIRBRIDGE_PASSWORD_FILE_MAX 8192U

typedef struct {
    char name[AIRBRIDGE_PASSWORD_NAME + 1];
    char value[AIRBRIDGE_PASSWORD_SIZE + 1];
} AirbridgePassword;

typedef struct {
    AirbridgePassword entries[AIRBRIDGE_PASSWORD_COUNT];
    size_t count;
} AirbridgePasswords;

/* Reject the whole file on malformed input; never type a truncated value. */
bool airbridge_passwords_parse(AirbridgePasswords* passwords, const char* data, size_t size);
void airbridge_passwords_clear(AirbridgePasswords* passwords);
