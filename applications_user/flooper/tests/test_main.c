#include "../flooper_app.h"
#include "fakes/storage/storage.h"
#include <stdio.h>
#include <string.h>

_Static_assert(FLOOPER_CATALOG_COUNT == 2, "two catalog entries required");
_Static_assert(FLOOPER_WORKER_STACK_SIZE == 4096, "initial worker stack");

int main(void) {
    if(strcmp(flooper_catalog[0].display_name, "BULERIAS") != 0 ||
       strcmp(flooper_catalog[1].display_name, "TANGOS") != 0 ||
       strcmp(flooper_catalog[0].filename, "flipper_bulerias_pattern_v2_1.json") != 0 ||
       strcmp(flooper_catalog[1].filename, "flipper_tangos_pattern_v2_1.json") != 0) {
        return 1;
    }
    for(unsigned int i = 0; i < FLOOPER_CATALOG_COUNT; ++i) {
        if(strlen(flooper_catalog[i].filename) >= FLOOPER_CATALOG_FILENAME_MAX ||
           strchr(flooper_catalog[i].filename, '/') != NULL) {
            return 1;
        }
    }
    if(strcmp(APP_ASSETS_PATH(""), "/assets/") != 0 ||
       strcmp(APP_ASSETS_PATH("pattern.json"), "/assets/pattern.json") != 0) {
        return 1;
    }
    puts("catalog: PASS (2 records); assets macro: PASS");
    return 0;
}
