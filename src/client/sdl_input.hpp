#pragma once

#include <cstdint>

#include <SDL2/SDL.h>

namespace waydisplay {

uint16_t sdl_scancode_to_evdev(SDL_Scancode scancode);

} // namespace waydisplay
