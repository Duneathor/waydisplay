#include "waydisplay/wd_keyboard_state.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL: %s\n", #expr); std::exit(1); } } while (0)

int main() {
    const uint32_t pressed[] = {29, 42};
    CHECK(wd_key_transition_classify(pressed, 2, 3, 29, true) == WD_KEY_TRANSITION_DUPLICATE_PRESS);
    CHECK(wd_key_transition_classify(pressed, 2, 3, 29, false) == WD_KEY_TRANSITION_ACCEPT);
    CHECK(wd_key_transition_classify(pressed, 2, 3, 30, false) == WD_KEY_TRANSITION_UNMATCHED_RELEASE);
    CHECK(wd_key_transition_classify(pressed, 2, 3, 30, true) == WD_KEY_TRANSITION_ACCEPT);
    CHECK(wd_key_transition_classify(pressed, 2, 2, 30, true) == WD_KEY_TRANSITION_CAPACITY);
    CHECK(wd_key_transition_classify(nullptr, 0, 3, 30, true) == WD_KEY_TRANSITION_ACCEPT);
    CHECK(wd_key_transition_classify(nullptr, 1, 3, 30, true) == WD_KEY_TRANSITION_CAPACITY);
    return 0;
}
