#include "../flooper_pattern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FlooperPattern document;
static FlooperPatternWorkspace workspace;
static char input[FLOOPER_JSON_BYTES + 2];

static int golden(const char* mode) {
    if(strcmp(mode, "bulerias") == 0 || strcmp(mode, "legacy") == 0) {
        if(strcmp(document.sections[0].name, "INTRO") ||
           strcmp(document.sections[1].name, "PALMAS") ||
           strcmp(document.sections[2].name, "PALMAS+PERC"))
            return 1;
        if(document.count_count != 6 || document.labels[0] != 6 || document.pulse_us != 314667 ||
           document.cycle_us != 1888002 || document.section_count != 3 ||
           document.sections[2].cycles != 12 || document.event_count != 20 ||
           document.events[7].offset_us != 42000 || document.events[12].offset_us != 157334)
            return 1;
    }
    if(strcmp(mode, "tangos") == 0 || strcmp(mode, "draft") == 0) {
        bool draft = strcmp(mode, "draft") == 0;
        if(document.count_count != 4 || document.labels[0] != (draft ? 1 : 4) ||
           document.cycle_us != 2879276 || document.event_count != 16 ||
           document.events[10].offset_us != 389000 || document.events[10].voice != 2 ||
           document.events[10].source_intensity < 0.2529f ||
           document.events[10].source_intensity > 0.2531f ||
           document.warning_cycle_mismatch != draft)
            return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if(argc != 4) return 2;
    FILE* file = fopen(argv[1], "rb");
    if(!file) return 2;
    size_t length = fread(input, 1, sizeof(input), file);
    int failed = ferror(file);
    fclose(file);
    if(failed) return 2;
    memset(&document, 0xa5, sizeof(document));
    FlooperPatternError error = flooper_pattern_parse(input, length, &workspace, &document);
    if(strcmp(flooper_pattern_error_name(error), argv[2]) != 0) {
        fprintf(stderr, "expected %s, got %s\n", argv[2], flooper_pattern_error_name(error));
        return 1;
    }
    if(error == FlooperPatternOk) {
        if(golden(argv[3])) return 1;
        if(document.warning_cycle_mismatch)
            fprintf(
                stderr,
                "warning: ignored draft cycle_us=%u; derived cycle_us=%llu\n",
                document.reported_cycle_us,
                (unsigned long long)document.cycle_us);
    } else {
        const unsigned char* bytes = (const unsigned char*)&document;
        for(size_t i = 0; i < sizeof(document); ++i)
            if(bytes[i]) return 1;
    }
    /* Given a prior document; when the next load fails; then no stale model survives. */
    if(flooper_pattern_parse("{", 1, &workspace, &document) != FlooperPatternJsonSyntax ||
       document.count_count != 0 || document.event_count != 0)
        return 1;
    printf("case: PASS expected=%s; cases=1 failures=0\n", argv[2]);
    return 0;
}
