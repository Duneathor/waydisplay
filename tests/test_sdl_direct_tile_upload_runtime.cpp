#include "client_state.hpp"
#include "sdl_direct_tile_upload.hpp"
#include "stream_ownership.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s (%s)\n", __FILE__, __LINE__, #condition, SDL_GetError());                          \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

waydisplay::ClientTileUpload make_upload(uint64_t ownership_epoch) {
    waydisplay::ClientTileUpload upload{};
    upload.rect = {1, 1, 2, 2};
    upload.ownership_epoch = ownership_epoch;
    upload.generation = 7;
    upload.source_pitch = 2u * sizeof(uint32_t);
    const uint32_t pixels[] = {
        UINT32_C(0xff112233), UINT32_C(0xff445566),
        UINT32_C(0xff778899), UINT32_C(0xffaabbcc),
    };
    upload.pixels.resize(sizeof(pixels));
    std::memcpy(upload.pixels.data(), pixels, sizeof(pixels));
    return upload;
}

uint32_t surface_pixel_argb8888(SDL_Surface* surface, int x, int y) {
    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    CHECK(converted != nullptr);
    const auto* row = static_cast<const uint8_t*>(converted->pixels) + static_cast<size_t>(y) * converted->pitch;
    uint32_t value = 0;
    std::memcpy(&value, row + static_cast<size_t>(x) * sizeof(value), sizeof(value));
    SDL_DestroySurface(converted);
    return value;
}

} // namespace

int main() {
    (void)setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SKIP: SDL dummy video unavailable: %s\n", SDL_GetError());
        return 77;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("waydisplay-direct-tile-test", 4, 4, SDL_WINDOW_HIDDEN, &window, &renderer))
    {
        std::fprintf(stderr, "SKIP: SDL renderer unavailable: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 4, 4);
    CHECK(texture != nullptr);

    waydisplay::ClientState state{};
    state.config.width = 4;
    state.config.height = 4;
    state.framebuffer.assign(16, 0);
    const auto ownership = wd_client_stream_ownership_snapshot(&state.stream_ownership);
    CHECK(ownership.owner == WD_CLIENT_CONTENT_OWNER_TILES);

    std::vector<waydisplay::ClientTileUpload> uploads;
    uploads.push_back(make_upload(ownership.epoch));
    std::vector<waydisplay::ClientDirtyRect> uploaded_rects;
    CHECK(waydisplay::upload_completed_tiles_direct(state, texture, uploads, uploaded_rects));
    CHECK(uploaded_rects.size() == 1);
    CHECK(uploaded_rects[0].x == 1 && uploaded_rects[0].y == 1 && uploaded_rects[0].w == 2 && uploaded_rects[0].h == 2);
    CHECK(state.stats.tile_present_direct.load(std::memory_order_relaxed) == 1);
    CHECK(state.framebuffer[5] == UINT32_C(0xff112233));
    CHECK(state.framebuffer[6] == UINT32_C(0xff445566));
    CHECK(state.framebuffer[9] == UINT32_C(0xff778899));
    CHECK(state.framebuffer[10] == UINT32_C(0xffaabbcc));

    CHECK(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255));
    CHECK(SDL_RenderClear(renderer));
    CHECK(SDL_RenderTexture(renderer, texture, nullptr, nullptr));
    CHECK(SDL_RenderPresent(renderer));
    SDL_Surface* readback = SDL_RenderReadPixels(renderer, nullptr);
    CHECK(readback != nullptr);
    CHECK(surface_pixel_argb8888(readback, 1, 1) == UINT32_C(0xff112233));
    CHECK(surface_pixel_argb8888(readback, 2, 2) == UINT32_C(0xffaabbcc));
    SDL_DestroySurface(readback);

    waydisplay::recycle_direct_tile_upload_buffers(state, uploads);
    CHECK(uploads[0].pixels.empty());
    CHECK(state.tile_present_recycled_buffers.size() == 1);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
