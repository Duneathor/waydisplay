#include "sdl_input.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL: %s\n", #expr); std::exit(1); } } while (0)

int main() {
    using waydisplay::sdl_scancode_to_evdev;

    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_NONUSBACKSLASH) == 86);  // KEY_102ND
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_PRINTSCREEN) == 99);     // KEY_SYSRQ
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_KP_EQUALS) == 117);      // KEY_KPEQUAL
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_PAUSE) == 119);          // KEY_PAUSE
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_UNKNOWN) == 0);
    return 0;
}
