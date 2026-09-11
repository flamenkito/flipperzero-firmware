#include "ui_fake.h"
int main(int argc, char** argv) {
    assert(argc == 3);
    if(strcmp(argv[2], "ui") == 0) {
        test_ui_render();
        test_ui_app(argv[1]);
        printf("ui: PASS cases=%u failures=0\n", ui_cases);
    } else {
        assert(strcmp(argv[2], "adapters") == 0);
        test_adapters_app(argv[1]);
        printf("adapters: PASS cases=%u failures=0\n", ui_cases);
    }
    return 0;
}
