#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux evdev button codes carried by the WayDisplay input protocol. */
#define WD_INPUT_BUTTON_LEFT   UINT16_C(0x110)
#define WD_INPUT_BUTTON_RIGHT  UINT16_C(0x111)
#define WD_INPUT_BUTTON_MIDDLE UINT16_C(0x112)
#define WD_INPUT_BUTTON_SIDE   UINT16_C(0x113)
#define WD_INPUT_BUTTON_EXTRA  UINT16_C(0x114)

/* XKB keycodes are evdev keycodes offset by eight. */
#define WD_INPUT_XKB_KEYCODE_OFFSET UINT32_C(8)

#ifdef __cplusplus
}
#endif
