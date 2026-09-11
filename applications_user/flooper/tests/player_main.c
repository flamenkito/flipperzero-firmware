#include "player_fake.h"

int main(int argc, char** argv) {
    assert(argc == 3);
    uint32_t hz = (uint32_t)strtoul(argv[2], NULL, 10);
    assert(hz == 1000 || hz == 1024);
    fake_reset(hz, 0);
    test_clock();
    test_transitions();
    test_loading(argv[1]);
    test_limits();
    test_quit();
    assert(player_cases > 0 && fake.joined);
    printf("player: PASS hz=%u cases=%u failures=0\n", hz, player_cases);
    return 0;
}
