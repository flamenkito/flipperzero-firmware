#include "../flooper_pattern.h"
#include <stdio.h>
#include <string.h>

static FlooperPattern a, b;
static FlooperPatternWorkspace workspace;
static char input[FLOOPER_JSON_BYTES + 1];

static bool load(const char* path, FlooperPattern* out) {
    FILE* file = fopen(path, "rb");
    if(!file) return false;
    size_t size = fread(input, 1, sizeof(input), file);
    bool failed = ferror(file) != 0;
    fclose(file);
    return !failed && flooper_pattern_parse(input, size, &workspace, out) == FlooperPatternOk;
}

int main(int argc, char** argv) {
    if(argc != 4 || !load(argv[1], &a) || !load(argv[2], &b)) return 1;
    if(strcmp(argv[3], "draft") == 0) {
        const uint8_t labels[] = {1, 2, 3, 4};
        if(memcmp(b.labels, labels, sizeof(labels)) || !b.warning_cycle_mismatch ||
           b.reported_cycle_us != 2879274)
            return 1;
        memcpy(b.labels, a.labels, sizeof(a.labels));
        b.accent_mask = a.accent_mask;
        b.warning_cycle_mismatch = false;
        b.reported_cycle_us = 0;
    }
    if(memcmp(&a, &b, sizeof(a))) return 1;
    memset(&workspace, 0xa5, sizeof(workspace));
    if(a.voices[0].slices[0].duration_us == 0 || a.sections[0].cycles == 0) return 1;
    puts("equivalence: PASS cases=1 failures=0");
    return 0;
}
