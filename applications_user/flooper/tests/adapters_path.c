#include "player_fake.h"
#include "../flooper_app.h"
#include <stdarg.h>

static bool negative;
static char filename[FLOOPER_CATALOG_FILENAME_MAX + 1];
const FlooperCatalogEntry path_catalog[FLOOPER_CATALOG_COUNT] = {
    {"TEST", filename},
    {"TEST", filename}};
static int path_snprintf(char* output, size_t size, const char* format, ...) {
    if(negative) return -1;
    va_list args;
    va_start(args, format);
    int result = vsnprintf(output, size, format, args);
    va_end(args);
    return result;
}
/* Exercise the actual private loader with adversarial catalog/stdio boundaries. */
#define flooper_catalog path_catalog
#ifdef snprintf
#undef snprintf
#endif
#define snprintf path_snprintf
#include "../flooper_player.c"
#undef snprintf
#undef flooper_catalog

int main(void) {
    for(unsigned mode = 0; mode < 4; ++mode) {
        /* Given a maximum valid basename, truncated basename, encoding failure,
         * or false open; When the real loader runs; Then reject before open or
         * close the attempted open before free, with typed Storage error. */
        fake_reset(1000, 0);
        memset(filename, 'a', sizeof(filename));
        filename[mode == 1 ? FLOOPER_CATALOG_FILENAME_MAX : FLOOPER_CATALOG_FILENAME_MAX - 1] = 0;
        negative = mode == 2;
        char expected[100];
        snprintf(expected, sizeof(expected), "/assets/%s", filename);
        fake.expected_path = expected;
        fake.open_fail = true;
        FlooperPlayer player = {0};
        LoadWorkspace* work = calloc(1, sizeof(*work));
        assert(work);
        fake_worker = true;
        assert(read_document(&player, work) == FlooperPlayerStorage);
        fake_worker = false;
        bool rejected = mode == 1 || mode == 2;
        assert(fake.opens == (rejected ? 0U : 1U));
        assert(fake.closes == fake.opens);
        assert(fake.file_count == 0 && fake.record_count == 0);
        free(work);
    }
    puts("adapters-path: PASS cases=4 failures=0");
    return 0;
}
