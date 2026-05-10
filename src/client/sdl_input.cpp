#include "sdl_input.hpp"

namespace waydisplay {

uint16_t sdl_scancode_to_evdev(SDL_Scancode scancode) {
    /*
     * SDL scancodes are USB-HID-like.
     * wlroots expects Linux evdev keycodes.
     * Unknown keys return 0 and are ignored.
     */
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE: return 1;

        case SDL_SCANCODE_1: return 2;
        case SDL_SCANCODE_2: return 3;
        case SDL_SCANCODE_3: return 4;
        case SDL_SCANCODE_4: return 5;
        case SDL_SCANCODE_5: return 6;
        case SDL_SCANCODE_6: return 7;
        case SDL_SCANCODE_7: return 8;
        case SDL_SCANCODE_8: return 9;
        case SDL_SCANCODE_9: return 10;
        case SDL_SCANCODE_0: return 11;

        case SDL_SCANCODE_MINUS: return 12;
        case SDL_SCANCODE_EQUALS: return 13;
        case SDL_SCANCODE_BACKSPACE: return 14;
        case SDL_SCANCODE_TAB: return 15;

        case SDL_SCANCODE_Q: return 16;
        case SDL_SCANCODE_W: return 17;
        case SDL_SCANCODE_E: return 18;
        case SDL_SCANCODE_R: return 19;
        case SDL_SCANCODE_T: return 20;
        case SDL_SCANCODE_Y: return 21;
        case SDL_SCANCODE_U: return 22;
        case SDL_SCANCODE_I: return 23;
        case SDL_SCANCODE_O: return 24;
        case SDL_SCANCODE_P: return 25;

        case SDL_SCANCODE_LEFTBRACKET: return 26;
        case SDL_SCANCODE_RIGHTBRACKET: return 27;
        case SDL_SCANCODE_RETURN: return 28;
        case SDL_SCANCODE_LCTRL: return 29;

        case SDL_SCANCODE_A: return 30;
        case SDL_SCANCODE_S: return 31;
        case SDL_SCANCODE_D: return 32;
        case SDL_SCANCODE_F: return 33;
        case SDL_SCANCODE_G: return 34;
        case SDL_SCANCODE_H: return 35;
        case SDL_SCANCODE_J: return 36;
        case SDL_SCANCODE_K: return 37;
        case SDL_SCANCODE_L: return 38;

        case SDL_SCANCODE_SEMICOLON: return 39;
        case SDL_SCANCODE_APOSTROPHE: return 40;
        case SDL_SCANCODE_GRAVE: return 41;
        case SDL_SCANCODE_LSHIFT: return 42;
        case SDL_SCANCODE_BACKSLASH: return 43;

        case SDL_SCANCODE_Z: return 44;
        case SDL_SCANCODE_X: return 45;
        case SDL_SCANCODE_C: return 46;
        case SDL_SCANCODE_V: return 47;
        case SDL_SCANCODE_B: return 48;
        case SDL_SCANCODE_N: return 49;
        case SDL_SCANCODE_M: return 50;

        case SDL_SCANCODE_COMMA: return 51;
        case SDL_SCANCODE_PERIOD: return 52;
        case SDL_SCANCODE_SLASH: return 53;
        case SDL_SCANCODE_RSHIFT: return 54;
        case SDL_SCANCODE_KP_MULTIPLY: return 55;
        case SDL_SCANCODE_LALT: return 56;
        case SDL_SCANCODE_SPACE: return 57;
        case SDL_SCANCODE_CAPSLOCK: return 58;

        case SDL_SCANCODE_F1: return 59;
        case SDL_SCANCODE_F2: return 60;
        case SDL_SCANCODE_F3: return 61;
        case SDL_SCANCODE_F4: return 62;
        case SDL_SCANCODE_F5: return 63;
        case SDL_SCANCODE_F6: return 64;
        case SDL_SCANCODE_F7: return 65;
        case SDL_SCANCODE_F8: return 66;
        case SDL_SCANCODE_F9: return 67;
        case SDL_SCANCODE_F10: return 68;

        case SDL_SCANCODE_NUMLOCKCLEAR: return 69;
        case SDL_SCANCODE_SCROLLLOCK: return 70;

        case SDL_SCANCODE_KP_7: return 71;
        case SDL_SCANCODE_KP_8: return 72;
        case SDL_SCANCODE_KP_9: return 73;
        case SDL_SCANCODE_KP_MINUS: return 74;
        case SDL_SCANCODE_KP_4: return 75;
        case SDL_SCANCODE_KP_5: return 76;
        case SDL_SCANCODE_KP_6: return 77;
        case SDL_SCANCODE_KP_PLUS: return 78;
        case SDL_SCANCODE_KP_1: return 79;
        case SDL_SCANCODE_KP_2: return 80;
        case SDL_SCANCODE_KP_3: return 81;
        case SDL_SCANCODE_KP_0: return 82;
        case SDL_SCANCODE_KP_PERIOD: return 83;

        case SDL_SCANCODE_F11: return 87;
        case SDL_SCANCODE_F12: return 88;

        case SDL_SCANCODE_KP_ENTER: return 96;
        case SDL_SCANCODE_RCTRL: return 97;
        case SDL_SCANCODE_KP_DIVIDE: return 98;
        case SDL_SCANCODE_RALT: return 100;

        case SDL_SCANCODE_HOME: return 102;
        case SDL_SCANCODE_UP: return 103;
        case SDL_SCANCODE_PAGEUP: return 104;
        case SDL_SCANCODE_LEFT: return 105;
        case SDL_SCANCODE_RIGHT: return 106;
        case SDL_SCANCODE_END: return 107;
        case SDL_SCANCODE_DOWN: return 108;
        case SDL_SCANCODE_PAGEDOWN: return 109;
        case SDL_SCANCODE_INSERT: return 110;
        case SDL_SCANCODE_DELETE: return 111;

        case SDL_SCANCODE_LGUI: return 125;
        case SDL_SCANCODE_RGUI: return 126;

        default: return 0;
    }
}

} // namespace waydisplay
