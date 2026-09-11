#pragma once

/* Match storage.h token concatenation: runtime arguments must not compile. */
#define APP_ASSETS_PATH(path) "/assets/" path

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RECORD_STORAGE "storage"
typedef struct Storage Storage;
typedef struct File File;
typedef enum {
    FSAM_READ
} FS_AccessMode;
typedef enum {
    FSOM_OPEN_EXISTING
} FS_OpenMode;
File* storage_file_alloc(Storage* storage);
void storage_file_free(File* file);
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode);
bool storage_file_close(File* file);
size_t storage_file_read(File* file, void* buffer, size_t size);
uint64_t storage_file_size(File* file);
