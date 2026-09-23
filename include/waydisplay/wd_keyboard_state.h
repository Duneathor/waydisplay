#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_key_transition {
    WD_KEY_TRANSITION_ACCEPT,
    WD_KEY_TRANSITION_DUPLICATE_PRESS,
    WD_KEY_TRANSITION_UNMATCHED_RELEASE,
    WD_KEY_TRANSITION_CAPACITY,
};

/* Classify before changing XKB state or notifying a Wayland seat. The client
 * suppresses SDL repeats, so a second remote press is not a repeat event. */
static inline enum wd_key_transition wd_key_transition_classify(const uint32_t* pressed_keys, size_t count,
                                                                  size_t capacity, uint32_t keycode, bool pressed) {
    if (count > capacity || (count != 0 && !pressed_keys)) {
        return WD_KEY_TRANSITION_CAPACITY;
    }
    for (size_t i = 0; i < count; ++i) {
        if (pressed_keys[i] == keycode) {
            return pressed ? WD_KEY_TRANSITION_DUPLICATE_PRESS : WD_KEY_TRANSITION_ACCEPT;
        }
    }
    if (!pressed) {
        return WD_KEY_TRANSITION_UNMATCHED_RELEASE;
    }
    return count == capacity ? WD_KEY_TRANSITION_CAPACITY : WD_KEY_TRANSITION_ACCEPT;
}

#ifdef __cplusplus
}
#endif
