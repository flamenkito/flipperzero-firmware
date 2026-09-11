#pragma once

#include <stdint.h>

#define FLOOPER_CATALOG_COUNT        2U
#define FLOOPER_CATALOG_FILENAME_MAX 36U
#define FLOOPER_WORKER_STACK_SIZE    4096U

typedef struct {
    const char* display_name;
    const char* filename;
} FlooperCatalogEntry;

extern const FlooperCatalogEntry flooper_catalog[FLOOPER_CATALOG_COUNT];

int32_t flooper_app(void* context);
