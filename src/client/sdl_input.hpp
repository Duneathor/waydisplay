#pragma once

#include <SDL2/SDL.h>
#include <cstdint>

namespace waydisplay {

uint16_t sdl_scancode_to_evdev(SDL_Scancode scancode);

} // namespace waydisplay
