#include "sdl_viewer.hpp"

#include "client_net.hpp"
#include "client_async_udp.hpp"
#include "sdl_input.hpp"
#include "tile_reassembly.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <thread>
#include <iterator>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace waydisplay {
namespace {

const wd_server_config_payload*     g_client_config = nullptr;
int                                 g_window_width  = 1;
int                                 g_window_height = 1;
SDL_FRect                           g_content_rect{0.0f, 0.0f, 1.0f, 1.0f};
std::array<bool, SDL_SCANCODE_COUNT> g_forwarded_keys{};
bool                                g_suppress_paste_v_keyup = false;

constexpr uint16_t WD_BTN_LEFT   = 0x110;
constexpr uint16_t WD_BTN_RIGHT  = 0x111;
constexpr uint16_t WD_BTN_MIDDLE = 0x112;
constexpr uint16_t WD_BTN_SIDE   = 0x113;
constexpr uint16_t WD_BTN_EXTRA  = 0x114;

constexpr uint16_t WD_POINTER_MOD_ALT   = 1u << 0;
constexpr uint16_t WD_POINTER_MOD_SHIFT = 1u << 1;
constexpr uint16_t WD_POINTER_MOD_CTRL  = 1u << 2;
constexpr uint16_t WD_POINTER_MOD_SUPER = 1u << 3;

uint16_t client_local_present_fps(const ClientState& state) {
    uint16_t fps = state.stream_config.target_fps;
    if (fps == 0)
    {
        fps = WD_CLIENT_DEFAULT_TARGET_FPS;
    }
    if (fps < WD_STREAM_FPS_MIN)
    {
        fps = WD_STREAM_FPS_MIN;
    }
    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }
    return fps;
}

uint64_t client_local_present_interval_ns(const ClientState& state) {
    return WD_NSEC_PER_SEC / client_local_present_fps(state);
}

bool client_local_present_due(const ClientState& state, uint64_t last_present_ns, uint64_t now_ns) {
    if (last_present_ns == 0)
    {
        return true;
    }

    return now_ns - last_present_ns >= client_local_present_interval_ns(state);
}

uint32_t client_present_delay_ms(const ClientState& state, uint64_t last_present_ns, uint64_t now_ns) {
    if (last_present_ns == 0)
    {
        return 1;
    }

    const uint64_t interval_ns = client_local_present_interval_ns(state);
    const uint64_t elapsed_ns  = now_ns - last_present_ns;
    if (elapsed_ns >= interval_ns)
    {
        return 0;
    }

    uint64_t remaining_ms = (interval_ns - elapsed_ns + 999999ull) / 1000000ull;
    if (remaining_ms == 0)
    {
        remaining_ms = 1;
    }
    if (remaining_ms > WD_CLIENT_FRAME_DELAY_MS)
    {
        remaining_ms = WD_CLIENT_FRAME_DELAY_MS;
    }
    return static_cast<uint32_t>(remaining_ms);
}

void update_window_size(SDL_Window* window);

enum class ContextMenuAction {
    Disabled,
    ToggleFullscreen,
    ActualSize,
    Quit,
};

struct ContextMenuItem {
    const char*       label;
    ContextMenuAction action;
    bool              enabled;
};

struct ContextMenu {
    bool open  = false;
    int  x     = 0;
    int  y     = 0;
    int  hover = -1;
};

const char* sdl_error_or_unknown() {
    const char* error = SDL_GetError();
    return error && error[0] ? error : "unknown SDL error";
}

bool log_sdl_error(const char* action) {
    WD_LOG_ERROR("%s failed: %s", action, sdl_error_or_unknown());
    return false;
}

void log_sdl_warning(const char* action) {
    WD_LOG_WARN("%s failed: %s", action, sdl_error_or_unknown());
}

void client_stats_accumulate(ClientStatsSnapshot& dst, const ClientStatsSnapshot& src) {
    dst.udp_packets += src.udp_packets;
    dst.udp_bytes += src.udp_bytes;
    dst.udp_interarrival_samples += src.udp_interarrival_samples;
    dst.udp_interarrival_sum_ns += src.udp_interarrival_sum_ns;
    dst.udp_jitter_samples += src.udp_jitter_samples;
    dst.udp_jitter_sum_ns += src.udp_jitter_sum_ns;
    dst.udp_interarrival_max_ns = std::max(dst.udp_interarrival_max_ns, src.udp_interarrival_max_ns);
    dst.invalid += src.invalid;
    dst.ignored_probe += src.ignored_probe;
    dst.stale_session += src.stale_session;
    dst.old_gen += src.old_gen;
    dst.completed += src.completed;
    dst.completed_compressed += src.completed_compressed;
    dst.completed_packets += src.completed_packets;
    dst.partial_timeouts += src.partial_timeouts;
    dst.partial_missing_packets += src.partial_missing_packets;
    dst.partial_retx_queued += src.partial_retx_queued;
    dst.retx_response_samples += src.retx_response_samples;
    dst.retx_response_sum_ns += src.retx_response_sum_ns;
    dst.timeout_updates += src.timeout_updates;
    dst.summaries += src.summaries;
    dst.retx += src.retx;
    dst.summary_retx_queued += src.summary_retx_queued;
    dst.summary_retx_deferred += src.summary_retx_deferred;
    dst.summary_retx_throttled += src.summary_retx_throttled;
    dst.summary_retx_stale_dropped += src.summary_retx_stale_dropped;
    dst.summary_retx_pressure_dropped += src.summary_retx_pressure_dropped;
    dst.summary_promote_passes += src.summary_promote_passes;
    dst.summary_promote_scanned += src.summary_promote_scanned;
    dst.summary_promote_candidates += src.summary_promote_candidates;
    dst.summary_to_retx_samples += src.summary_to_retx_samples;
    dst.summary_to_retx_sum_ns += src.summary_to_retx_sum_ns;
    dst.keys += src.keys;
    dst.pointer += src.pointer;
    dst.input_events += src.input_events;
    dst.input_channel_events += src.input_channel_events;
    dst.input_fallback_events += src.input_fallback_events;
    dst.selection_channel_events += src.selection_channel_events;
    dst.selection_fallback_events += src.selection_fallback_events;
    dst.tcp_async_queued += src.tcp_async_queued;
    dst.tcp_async_completed += src.tcp_async_completed;
    dst.tcp_async_failed += src.tcp_async_failed;
    dst.tcp_async_overflow += src.tcp_async_overflow;
    dst.tcp_async_partial += src.tcp_async_partial;
    dst.tcp_async_coalesced += src.tcp_async_coalesced;
    dst.tcp_async_inflight_max = std::max(dst.tcp_async_inflight_max, src.tcp_async_inflight_max);
    dst.udp_async_posted += src.udp_async_posted;
    dst.udp_async_completed += src.udp_async_completed;
    dst.udp_async_failed += src.udp_async_failed;
    dst.udp_async_submit_failed += src.udp_async_submit_failed;
    dst.udp_async_cancels += src.udp_async_cancels;
    dst.udp_async_inflight_max = std::max(dst.udp_async_inflight_max, src.udp_async_inflight_max);
    dst.tile_assembly_samples += src.tile_assembly_samples;
    dst.tile_assembly_sum_ns += src.tile_assembly_sum_ns;
    dst.tile_present_samples += src.tile_present_samples;
    dst.tile_present_sum_ns += src.tile_present_sum_ns;
    dst.input_to_present_samples += src.input_to_present_samples;
    dst.input_to_present_sum_ns += src.input_to_present_sum_ns;
    dst.input_seq_present_samples += src.input_seq_present_samples;
    dst.input_seq_present_sum_ns += src.input_seq_present_sum_ns;
    dst.sdl_render_frames += src.sdl_render_frames;
    dst.sdl_remote_frames += src.sdl_remote_frames;
    dst.sdl_empty_remote_wakeups += src.sdl_empty_remote_wakeups;
    dst.sdl_texture_full_uploads += src.sdl_texture_full_uploads;
    dst.sdl_texture_partial_uploads += src.sdl_texture_partial_uploads;
    dst.sdl_texture_dirty_rects += src.sdl_texture_dirty_rects;
    dst.sdl_texture_source_dirty_rects += src.sdl_texture_source_dirty_rects;
    dst.sdl_texture_coalesced_dirty_rects += src.sdl_texture_coalesced_dirty_rects;
    dst.sdl_texture_bounds_uploads += src.sdl_texture_bounds_uploads;
    dst.sdl_texture_upload_pixels += src.sdl_texture_upload_pixels;
    dst.sdl_texture_upload_samples += src.sdl_texture_upload_samples;
    dst.sdl_texture_upload_sum_ns += src.sdl_texture_upload_sum_ns;
    dst.sdl_texture_upload_max_ns = std::max(dst.sdl_texture_upload_max_ns, src.sdl_texture_upload_max_ns);
    dst.sdl_present_samples += src.sdl_present_samples;
    dst.sdl_present_sum_ns += src.sdl_present_sum_ns;
    dst.sdl_present_max_ns = std::max(dst.sdl_present_max_ns, src.sdl_present_max_ns);
}

const std::array<ContextMenuItem, 5> CONTEXT_MENU_ITEMS{{
    {"COPY FROM REMOTE", ContextMenuAction::Disabled, false},
    {"PASTE TO REMOTE", ContextMenuAction::Disabled, false},
    {"TOGGLE FULLSCREEN", ContextMenuAction::ToggleFullscreen, true},
    {"ACTUAL SIZE", ContextMenuAction::ActualSize, true},
    {"DISCONNECT", ContextMenuAction::Quit, true},
}};

int context_menu_height() {
    return WD_CLIENT_CONTEXT_MENU_PADDING_Y * 2 + static_cast<int>(CONTEXT_MENU_ITEMS.size()) * WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT;
}

void close_context_menu(ContextMenu& menu) {
    menu.open  = false;
    menu.hover = -1;
}

void open_context_menu(ContextMenu& menu, float x, float y) {
    menu.open  = true;
    menu.x     = static_cast<int>(x);
    menu.y     = static_cast<int>(y);
    menu.hover = -1;

    if (menu.x + WD_CLIENT_CONTEXT_MENU_WIDTH > g_window_width)
    {
        menu.x = g_window_width - WD_CLIENT_CONTEXT_MENU_WIDTH;
    }

    if (menu.y + context_menu_height() > g_window_height)
    {
        menu.y = g_window_height - context_menu_height();
    }

    if (menu.x < 0)
    {
        menu.x = 0;
    }

    if (menu.y < 0)
    {
        menu.y = 0;
    }
}

int context_menu_hit_test(const ContextMenu& menu, float x, float y) {
    if (!menu.open)
    {
        return -1;
    }

    if (x < menu.x || x >= menu.x + WD_CLIENT_CONTEXT_MENU_WIDTH)
    {
        return -1;
    }

    const int content_y = static_cast<int>(y) - menu.y - WD_CLIENT_CONTEXT_MENU_PADDING_Y;
    if (content_y < 0)
    {
        return -1;
    }

    const int index = content_y / WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT;
    if (index < 0 || index >= static_cast<int>(CONTEXT_MENU_ITEMS.size()))
    {
        return -1;
    }

    return index;
}

const char* glyph_5x7(char c) {
    switch (c)
    {
    case 'A':
        return "01110"
               "10001"
               "10001"
               "11111"
               "10001"
               "10001"
               "10001";
    case 'B':
        return "11110"
               "10001"
               "10001"
               "11110"
               "10001"
               "10001"
               "11110";
    case 'C':
        return "01111"
               "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "01111";
    case 'D':
        return "11110"
               "10001"
               "10001"
               "10001"
               "10001"
               "10001"
               "11110";
    case 'E':
        return "11111"
               "10000"
               "10000"
               "11110"
               "10000"
               "10000"
               "11111";
    case 'F':
        return "11111"
               "10000"
               "10000"
               "11110"
               "10000"
               "10000"
               "10000";
    case 'G':
        return "01111"
               "10000"
               "10000"
               "10011"
               "10001"
               "10001"
               "01110";
    case 'H':
        return "10001"
               "10001"
               "10001"
               "11111"
               "10001"
               "10001"
               "10001";
    case 'I':
        return "11111"
               "00100"
               "00100"
               "00100"
               "00100"
               "00100"
               "11111";
    case 'J':
        return "00111"
               "00010"
               "00010"
               "00010"
               "00010"
               "10010"
               "01100";
    case 'K':
        return "10001"
               "10010"
               "10100"
               "11000"
               "10100"
               "10010"
               "10001";
    case 'L':
        return "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "11111";
    case 'M':
        return "10001"
               "11011"
               "10101"
               "10101"
               "10001"
               "10001"
               "10001";
    case 'N':
        return "10001"
               "11001"
               "10101"
               "10011"
               "10001"
               "10001"
               "10001";
    case 'O':
        return "01110"
               "10001"
               "10001"
               "10001"
               "10001"
               "10001"
               "01110";
    case 'P':
        return "11110"
               "10001"
               "10001"
               "11110"
               "10000"
               "10000"
               "10000";
    case 'Q':
        return "01110"
               "10001"
               "10001"
               "10001"
               "10101"
               "10010"
               "01101";
    case 'R':
        return "11110"
               "10001"
               "10001"
               "11110"
               "10100"
               "10010"
               "10001";
    case 'S':
        return "01111"
               "10000"
               "10000"
               "01110"
               "00001"
               "00001"
               "11110";
    case 'T':
        return "11111"
               "00100"
               "00100"
               "00100"
               "00100"
               "00100"
               "00100";
    case 'U':
        return "10001"
               "10001"
               "10001"
               "10001"
               "10001"
               "10001"
               "01110";
    case 'V':
        return "10001"
               "10001"
               "10001"
               "10001"
               "10001"
               "01010"
               "00100";
    case 'W':
        return "10001"
               "10001"
               "10001"
               "10101"
               "10101"
               "10101"
               "01010";
    case 'X':
        return "10001"
               "10001"
               "01010"
               "00100"
               "01010"
               "10001"
               "10001";
    case 'Y':
        return "10001"
               "10001"
               "01010"
               "00100"
               "00100"
               "00100"
               "00100";
    case 'Z':
        return "11111"
               "00001"
               "00010"
               "00100"
               "01000"
               "10000"
               "11111";
    default:
        return nullptr;
    }
}

void draw_text(SDL_Renderer* renderer, const char* text, int x, int y, int scale, bool enabled) {
    if (enabled)
    {
        SDL_SetRenderDrawColor(renderer, 242, 242, 242, 255);
    }
    else
    {
        SDL_SetRenderDrawColor(renderer, 135, 135, 135, 255);
    }

    int cursor_x = x;
    for (const char* p = text; *p; ++p)
    {
        if (*p == ' ')
        {
            cursor_x += 4 * scale;
            continue;
        }

        const char* bits = glyph_5x7(*p);
        if (!bits)
        {
            cursor_x += 6 * scale;
            continue;
        }

        for (int row = 0; row < 7; ++row)
        {
            for (int col = 0; col < 5; ++col)
            {
                if (bits[row * 5 + col] != '1')
                {
                    continue;
                }

                SDL_FRect px{static_cast<float>(cursor_x + col * scale), static_cast<float>(y + row * scale),
                             static_cast<float>(scale), static_cast<float>(scale)};
                SDL_RenderFillRect(renderer, &px);
            }
        }

        cursor_x += 6 * scale;
    }
}

void render_context_menu(SDL_Renderer* renderer, const ContextMenu& menu) {
    if (!menu.open)
    {
        return;
    }

    const int height = context_menu_height();

    SDL_FRect shadow{static_cast<float>(menu.x + 5), static_cast<float>(menu.y + 5),
                     static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH), static_cast<float>(height)};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_RenderFillRect(renderer, &shadow);

    SDL_FRect bg{static_cast<float>(menu.x), static_cast<float>(menu.y),
                 static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH), static_cast<float>(height)};
    SDL_SetRenderDrawColor(renderer, 26, 29, 33, 248);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 72, 78, 86, 255);
    SDL_RenderRect(renderer, &bg);

    SDL_FRect inner{static_cast<float>(menu.x + 1), static_cast<float>(menu.y + 1),
                    static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH - 2), static_cast<float>(height - 2)};
    SDL_SetRenderDrawColor(renderer, 43, 47, 53, 255);
    SDL_RenderRect(renderer, &inner);

    for (int i = 0; i < static_cast<int>(CONTEXT_MENU_ITEMS.size()); ++i)
    {
        const SDL_FRect item_rect{
            static_cast<float>(menu.x + WD_CLIENT_CONTEXT_MENU_PADDING_X),
            static_cast<float>(menu.y + WD_CLIENT_CONTEXT_MENU_PADDING_Y + i * WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT),
            static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH - WD_CLIENT_CONTEXT_MENU_PADDING_X * 2),
            static_cast<float>(WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT),
        };

        if (i == menu.hover && CONTEXT_MENU_ITEMS[i].enabled)
        {
            SDL_SetRenderDrawColor(renderer, 56, 116, 186, 255);
            SDL_RenderFillRect(renderer, &item_rect);
        }

        if (i == 2)
        {
            SDL_SetRenderDrawColor(renderer, 66, 70, 76, 255);
            SDL_RenderLine(renderer, static_cast<float>(menu.x + WD_CLIENT_CONTEXT_MENU_PADDING_X), item_rect.y - 1.0f,
                           static_cast<float>(menu.x + WD_CLIENT_CONTEXT_MENU_WIDTH - WD_CLIENT_CONTEXT_MENU_PADDING_X),
                           item_rect.y - 1.0f);
        }

        draw_text(renderer, CONTEXT_MENU_ITEMS[i].label, static_cast<int>(item_rect.x) + WD_CLIENT_CONTEXT_MENU_TEXT_X,
                  static_cast<int>(item_rect.y) + WD_CLIENT_CONTEXT_MENU_TEXT_Y, WD_CLIENT_CONTEXT_MENU_TEXT_SCALE,
                  CONTEXT_MENU_ITEMS[i].enabled);
    }
}

void execute_context_menu_action(ClientState& state, SDL_Window* window, ContextMenuAction action) {
    switch (action)
    {
    case ContextMenuAction::ToggleFullscreen: {
        const SDL_WindowFlags flags      = SDL_GetWindowFlags(window);
        const bool            fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
        if (!SDL_SetWindowFullscreen(window, !fullscreen))
        {
            log_sdl_warning("SDL_SetWindowFullscreen");
        }
        update_window_size(window);
        break;
    }

    case ContextMenuAction::ActualSize:
        if (!SDL_SetWindowFullscreen(window, false))
        {
            log_sdl_warning("SDL_SetWindowFullscreen");
        }
        if (!SDL_SetWindowSize(window, state.config.width, state.config.height))
        {
            log_sdl_warning("SDL_SetWindowSize");
        }
        if (!SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED))
        {
            log_sdl_warning("SDL_SetWindowPosition");
        }
        update_window_size(window);
        break;

    case ContextMenuAction::Quit:
        state.running.store(false, std::memory_order_relaxed);
        break;

    case ContextMenuAction::Disabled:
    default:
        break;
    }
}

bool context_menu_open_gesture(const SDL_Event& event) {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_RIGHT)
    {
        return false;
    }

    const SDL_Keymod mods = SDL_GetModState();
    return (mods & SDL_KMOD_CTRL) && (mods & SDL_KMOD_ALT);
}

bool handle_context_menu_event(ClientState& state, SDL_Window* window, ContextMenu& menu, const SDL_Event& event, bool& out_frame_dirty) {
    if (context_menu_open_gesture(event))
    {
        open_context_menu(menu, event.button.x, event.button.y);
        out_frame_dirty = true;
        return true;
    }

    if (!menu.open)
    {
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION)
    {
        const int hover = context_menu_hit_test(menu, event.motion.x, event.motion.y);
        if (hover != menu.hover)
        {
            menu.hover      = hover;
            out_frame_dirty = true;
        }
        return true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
    {
        close_context_menu(menu);
        out_frame_dirty = true;
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
    {
        const int index = context_menu_hit_test(menu, event.button.x, event.button.y);
        if (index < 0)
        {
            close_context_menu(menu);
            out_frame_dirty = true;
            return true;
        }

        if (event.button.button == SDL_BUTTON_LEFT)
        {
            const ContextMenuItem& item = CONTEXT_MENU_ITEMS[index];
            close_context_menu(menu);
            out_frame_dirty = true;

            if (item.enabled)
            {
                execute_context_menu_action(state, window, item.action);
            }
            return true;
        }

        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL)
    {
        return true;
    }

    return false;
}

uint16_t current_pointer_modifiers() {
    const SDL_Keymod mods = SDL_GetModState();

    uint16_t result = 0;

    if (mods & SDL_KMOD_ALT)
    {
        result |= WD_POINTER_MOD_ALT;
    }

    if (mods & SDL_KMOD_SHIFT)
    {
        result |= WD_POINTER_MOD_SHIFT;
    }

    if (mods & SDL_KMOD_CTRL)
    {
        result |= WD_POINTER_MOD_CTRL;
    }

    if (mods & SDL_KMOD_GUI)
    {
        result |= WD_POINTER_MOD_SUPER;
    }

    return result;
}

bool send_host_clipboard_to_server(ClientState& state, bool primary) {
    char* text = primary ? SDL_GetPrimarySelectionText() : SDL_GetClipboardText();
    if (!text)
    {
        return false;
    }

    const bool ok = primary ? client_send_primary_text(state, text) : client_send_clipboard_text(state, text);

    SDL_free(text);
    return ok;
}

void drain_remote_selection_updates(ClientState& state) {
    std::string clipboard;
    std::string primary;
    bool        have_clipboard = false;
    bool        have_primary   = false;

    {
        std::lock_guard<std::mutex> lock(state.selection_mutex);

        if (state.pending_clipboard_text_valid)
        {
            clipboard = std::move(state.pending_clipboard_text);
            state.pending_clipboard_text.clear();
            state.pending_clipboard_text_valid = false;
            have_clipboard                     = true;
        }

        if (state.pending_primary_text_valid)
        {
            primary = std::move(state.pending_primary_text);
            state.pending_primary_text.clear();
            state.pending_primary_text_valid = false;
            have_primary                     = true;
        }
    }

    if (have_clipboard)
    {
        if (!SDL_SetClipboardText(clipboard.c_str()))
        {
            log_sdl_warning("SDL_SetClipboardText");
        }
    }

    if (have_primary)
    {
        if (!SDL_SetPrimarySelectionText(primary.c_str()))
        {
            log_sdl_warning("SDL_SetPrimarySelectionText");
        }
    }
}

SDL_SystemCursor sdl_cursor_for_wd_shape(uint16_t shape) {
    switch (shape)
    {
    case WD_CURSOR_SHAPE_POINTER:
        return SDL_SYSTEM_CURSOR_POINTER;

    case WD_CURSOR_SHAPE_TEXT:
        return SDL_SYSTEM_CURSOR_TEXT;

    case WD_CURSOR_SHAPE_MOVE:
        return SDL_SYSTEM_CURSOR_MOVE;

    case WD_CURSOR_SHAPE_EW_RESIZE:
        return SDL_SYSTEM_CURSOR_EW_RESIZE;

    case WD_CURSOR_SHAPE_NS_RESIZE:
        return SDL_SYSTEM_CURSOR_NS_RESIZE;

    case WD_CURSOR_SHAPE_NWSE_RESIZE:
        return SDL_SYSTEM_CURSOR_NWSE_RESIZE;

    case WD_CURSOR_SHAPE_NESW_RESIZE:
        return SDL_SYSTEM_CURSOR_NESW_RESIZE;

    case WD_CURSOR_SHAPE_WAIT:
        return SDL_SYSTEM_CURSOR_WAIT;

    case WD_CURSOR_SHAPE_NOT_ALLOWED:
        return SDL_SYSTEM_CURSOR_NOT_ALLOWED;

    case WD_CURSOR_SHAPE_DEFAULT:
    default:
        return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

void apply_pending_cursor_shape(ClientState& state) {
    static std::array<SDL_Cursor*, WD_CURSOR_SHAPE_COUNT> cursors{};
    static uint16_t                                       current_shape = 0xffffu;

    if (!state.pending_cursor_shape_dirty.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    uint16_t shape = state.pending_cursor_shape.load(std::memory_order_relaxed);

    if (shape >= WD_CURSOR_SHAPE_COUNT)
    {
        shape = WD_CURSOR_SHAPE_DEFAULT;
    }

    if (shape == current_shape)
    {
        return;
    }

    if (shape == WD_CURSOR_SHAPE_HIDDEN)
    {
        SDL_HideCursor();
        current_shape = shape;
        return;
    }

    if (!cursors[shape])
    {
        cursors[shape] = SDL_CreateSystemCursor(sdl_cursor_for_wd_shape(shape));
    }

    if (cursors[shape])
    {
        SDL_SetCursor(cursors[shape]);
        SDL_ShowCursor();
        current_shape = shape;
    }
}

void free_cached_cursors() {
    static_assert(WD_CURSOR_SHAPE_COUNT > 0, "cursor shape count must be nonzero");

    /*
     * Cursors are intentionally process-lifetime cached. SDL_Quit() will clean
     * them up; avoiding manual free also avoids ordering hazards with backends.
     */
}

uint64_t take_stat(std::atomic<uint64_t>& value) {
    return value.exchange(0, std::memory_order_relaxed);
}


void update_udp_gap_pressure(ClientState& state, uint64_t max_gap_ns, uint64_t interarrival_samples, uint64_t udp_packets) {
    uint64_t current = state.udp_gap_pressure_ns.load(std::memory_order_relaxed);
    uint64_t target  = 0;

    if (udp_packets >= 16 && interarrival_samples >= 16 && max_gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        target = max_gap_ns;
    }

    if (target > current)
    {
        state.udp_gap_pressure_ns.store(target, std::memory_order_relaxed);
        return;
    }

    if (current == 0)
    {
        return;
    }

    uint64_t decayed = (current * (uint64_t)WD_LINK_RUNTIME_GAP_PRESSURE_DECAY_PERCENT) / 100ull;
    if (decayed < WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        decayed = 0;
    }
    state.udp_gap_pressure_ns.store(decayed, std::memory_order_relaxed);
}

double avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return static_cast<double>(sum_ns) / static_cast<double>(samples) / 1000000.0;
}

void record_atomic_max(std::atomic<uint64_t>& value, uint64_t sample) {
    uint64_t current = value.load(std::memory_order_relaxed);

    while (sample > current && !value.compare_exchange_weak(current, sample, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

bool take_input_timestamp(ClientState& state, uint64_t sequence, uint64_t& timestamp_ns) {
    if (sequence == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(state.input_timestamp_mutex);

    for (auto it = state.recent_input_timestamps.begin(); it != state.recent_input_timestamps.end(); ++it)
    {
        if (it->sequence == sequence)
        {
            timestamp_ns = it->timestamp_ns;
            state.recent_input_timestamps.erase(state.recent_input_timestamps.begin(), std::next(it));
            return timestamp_ns != 0;
        }
    }

    return false;
}

void log_client_stats_snapshot(ClientState& state, const ClientStatsSnapshot& logged) {
    const uint64_t udp_packets = logged.udp_packets;
    const uint64_t udp_bytes = logged.udp_bytes;
    const uint64_t udp_interarrival_samples = logged.udp_interarrival_samples;
    const uint64_t udp_interarrival_sum_ns = logged.udp_interarrival_sum_ns;
    const uint64_t udp_jitter_samples = logged.udp_jitter_samples;
    const uint64_t udp_jitter_sum_ns = logged.udp_jitter_sum_ns;
    const uint64_t udp_interarrival_max_ns = logged.udp_interarrival_max_ns;
    const uint64_t invalid = logged.invalid;
    const uint64_t ignored_probe = logged.ignored_probe;
    const uint64_t stale_session = logged.stale_session;
    const uint64_t old_gen = logged.old_gen;
    const uint64_t completed = logged.completed;
    const uint64_t completed_compressed = logged.completed_compressed;
    const uint64_t completed_packets = logged.completed_packets;
    const uint64_t partial_timeouts = logged.partial_timeouts;
    const uint64_t partial_missing_packets = logged.partial_missing_packets;
    const uint64_t partial_retx_queued = logged.partial_retx_queued;
    const uint64_t retx_response_samples = logged.retx_response_samples;
    const uint64_t retx_response_sum_ns = logged.retx_response_sum_ns;
    const uint64_t timeout_updates = logged.timeout_updates;
    const uint64_t summaries = logged.summaries;
    const uint64_t retx = logged.retx;
    const uint64_t summary_retx_queued = logged.summary_retx_queued;
    const uint64_t summary_retx_deferred = logged.summary_retx_deferred;
    const uint64_t summary_retx_throttled = logged.summary_retx_throttled;
    const uint64_t summary_retx_stale_dropped = logged.summary_retx_stale_dropped;
    const uint64_t summary_retx_pressure_dropped = logged.summary_retx_pressure_dropped;
    const uint64_t summary_promote_passes = logged.summary_promote_passes;
    const uint64_t summary_promote_scanned = logged.summary_promote_scanned;
    const uint64_t summary_promote_candidates = logged.summary_promote_candidates;
    const uint64_t summary_to_retx_samples = logged.summary_to_retx_samples;
    const uint64_t summary_to_retx_sum_ns = logged.summary_to_retx_sum_ns;
    const uint64_t keys = logged.keys;
    const uint64_t pointer = logged.pointer;
    const uint64_t input_events = logged.input_events;
    const uint64_t input_channel_events = logged.input_channel_events;
    const uint64_t input_fallback_events = logged.input_fallback_events;
    const uint64_t selection_channel_events = logged.selection_channel_events;
    const uint64_t selection_fallback_events = logged.selection_fallback_events;
    const uint64_t tcp_async_queued = logged.tcp_async_queued;
    const uint64_t tcp_async_completed = logged.tcp_async_completed;
    const uint64_t tcp_async_failed = logged.tcp_async_failed;
    const uint64_t tcp_async_overflow = logged.tcp_async_overflow;
    const uint64_t tcp_async_partial = logged.tcp_async_partial;
    const uint64_t tcp_async_coalesced = logged.tcp_async_coalesced;
    const uint64_t tcp_async_inflight_max = logged.tcp_async_inflight_max;
    const uint64_t udp_async_posted = logged.udp_async_posted;
    const uint64_t udp_async_completed = logged.udp_async_completed;
    const uint64_t udp_async_failed = logged.udp_async_failed;
    const uint64_t udp_async_submit_failed = logged.udp_async_submit_failed;
    const uint64_t udp_async_cancels = logged.udp_async_cancels;
    const uint64_t udp_async_inflight_max = logged.udp_async_inflight_max;
    const uint64_t tile_assembly_samples = logged.tile_assembly_samples;
    const uint64_t tile_assembly_sum_ns = logged.tile_assembly_sum_ns;
    const uint64_t tile_present_samples = logged.tile_present_samples;
    const uint64_t tile_present_sum_ns = logged.tile_present_sum_ns;
    const uint64_t input_to_present_samples = logged.input_to_present_samples;
    const uint64_t input_to_present_sum_ns = logged.input_to_present_sum_ns;
    const uint64_t input_seq_present_samples = logged.input_seq_present_samples;
    const uint64_t input_seq_present_sum_ns = logged.input_seq_present_sum_ns;
    const uint64_t sdl_render_frames = logged.sdl_render_frames;
    const uint64_t sdl_remote_frames = logged.sdl_remote_frames;
    const uint64_t sdl_empty_remote_wakeups = logged.sdl_empty_remote_wakeups;
    const uint64_t sdl_texture_full_uploads = logged.sdl_texture_full_uploads;
    const uint64_t sdl_texture_partial_uploads = logged.sdl_texture_partial_uploads;
    const uint64_t sdl_texture_dirty_rects = logged.sdl_texture_dirty_rects;
    const uint64_t sdl_texture_source_dirty_rects = logged.sdl_texture_source_dirty_rects;
    const uint64_t sdl_texture_coalesced_dirty_rects = logged.sdl_texture_coalesced_dirty_rects;
    const uint64_t sdl_texture_bounds_uploads = logged.sdl_texture_bounds_uploads;
    const uint64_t sdl_texture_upload_pixels = logged.sdl_texture_upload_pixels;
    const uint64_t sdl_texture_upload_samples = logged.sdl_texture_upload_samples;
    const uint64_t sdl_texture_upload_sum_ns = logged.sdl_texture_upload_sum_ns;
    const uint64_t sdl_texture_upload_max_ns = logged.sdl_texture_upload_max_ns;
    const uint64_t sdl_present_samples = logged.sdl_present_samples;
    const uint64_t sdl_present_sum_ns = logged.sdl_present_sum_ns;
    const uint64_t sdl_present_max_ns = logged.sdl_present_max_ns;
    const uint64_t udp_gap_pressure_ms = state.udp_gap_pressure_ns.load(std::memory_order_relaxed) / 1000000ull;

    const bool udp_activity = udp_packets != 0 || udp_bytes != 0 || completed != 0 || invalid != 0 || old_gen != 0 ||
                              ignored_probe != 0 || stale_session != 0 || udp_async_posted != 0 ||
                              udp_async_completed != 0 || udp_async_failed != 0 || udp_async_submit_failed != 0 ||
                              udp_async_cancels != 0 || udp_async_inflight_max != 0;
    if (udp_activity)
    {
        WD_LOG_DEBUG("[client udp/min] pkts=%llu kib=%.1f completed=%llu invalid=%llu probe=%llu stale_session=%llu old_gen=%llu async_recv_submitted=%llu async_recv_completed=%llu async_recv_failed=%llu async_recv_submit_failed=%llu async_recv_cancels=%llu async_recv_inflight_max=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f kib_per_tile=%.2f compressed_kib_per_tile=%.2f pkts_per_tile=%.2f",
                     static_cast<unsigned long long>(udp_packets), static_cast<double>(udp_bytes) / 1024.0,
                     static_cast<unsigned long long>(completed), static_cast<unsigned long long>(invalid),
                     static_cast<unsigned long long>(ignored_probe), static_cast<unsigned long long>(stale_session),
                     static_cast<unsigned long long>(old_gen),
                     static_cast<unsigned long long>(udp_async_posted),
                     static_cast<unsigned long long>(udp_async_completed),
                     static_cast<unsigned long long>(udp_async_failed),
                     static_cast<unsigned long long>(udp_async_submit_failed),
                     static_cast<unsigned long long>(udp_async_cancels),
                     static_cast<unsigned long long>(udp_async_inflight_max),
                     avg_ms(udp_interarrival_sum_ns, udp_interarrival_samples),
                     avg_ms(udp_jitter_sum_ns, udp_jitter_samples), static_cast<double>(udp_interarrival_max_ns) / 1000000.0,
                     completed ? (static_cast<double>(udp_bytes) / 1024.0) / static_cast<double>(completed) : 0.0,
                     completed ? (static_cast<double>(completed_compressed) / 1024.0) / static_cast<double>(completed) : 0.0,
                     completed ? static_cast<double>(completed_packets) / static_cast<double>(completed) : 0.0);
    }

    const bool repair_activity = partial_timeouts != 0 || partial_missing_packets != 0 || partial_retx_queued != 0 ||
                                 summaries != 0 || retx != 0 || summary_retx_queued != 0 || summary_retx_deferred != 0 ||
                                 summary_retx_throttled != 0 || summary_retx_stale_dropped != 0 ||
                                 summary_retx_pressure_dropped != 0 || summary_promote_passes != 0 ||
                                 summary_to_retx_samples != 0 || retx_response_samples != 0;
    if (repair_activity)
    {
        WD_LOG_DEBUG("[client repair/min] summaries=%llu retx_req=%llu summary_retx_tiles=%llu summary_deferred=%llu summary_throttled=%llu stale_drop=%llu pressure_drop=%llu summary_promote=%llu summary_scan=%llu summary_candidates=%llu partial_timeouts=%llu missing_pkts=%llu partial_retx=%llu summary_to_retx_avg_ms=%.2f retx_response_avg_ms=%.2f",
                     static_cast<unsigned long long>(summaries), static_cast<unsigned long long>(retx),
                     static_cast<unsigned long long>(summary_retx_queued), static_cast<unsigned long long>(summary_retx_deferred),
                     static_cast<unsigned long long>(summary_retx_throttled),
                     static_cast<unsigned long long>(summary_retx_stale_dropped),
                     static_cast<unsigned long long>(summary_retx_pressure_dropped),
                     static_cast<unsigned long long>(summary_promote_passes),
                     static_cast<unsigned long long>(summary_promote_scanned),
                     static_cast<unsigned long long>(summary_promote_candidates),
                     static_cast<unsigned long long>(partial_timeouts),
                     static_cast<unsigned long long>(partial_missing_packets), static_cast<unsigned long long>(partial_retx_queued),
                     avg_ms(summary_to_retx_sum_ns, summary_to_retx_samples),
                     avg_ms(retx_response_sum_ns, retx_response_samples));
    }

    const bool input_activity = keys != 0 || pointer != 0 || input_events != 0 || input_channel_events != 0 ||
                                input_fallback_events != 0 || selection_channel_events != 0 || selection_fallback_events != 0 ||
                                tcp_async_coalesced != 0;
    if (input_activity)
    {
        WD_LOG_DEBUG("[client input/min] keys=%llu pointer_queued=%llu pointer_coalesced=%llu input_events_queued=%llu input_channel=%llu input_fallback=%llu selection_channel=%llu selection_fallback=%llu",
                     static_cast<unsigned long long>(keys), static_cast<unsigned long long>(pointer),
                     static_cast<unsigned long long>(tcp_async_coalesced), static_cast<unsigned long long>(input_events),
                     static_cast<unsigned long long>(input_channel_events), static_cast<unsigned long long>(input_fallback_events),
                     static_cast<unsigned long long>(selection_channel_events), static_cast<unsigned long long>(selection_fallback_events));
    }

    const bool tcp_async_activity = tcp_async_queued != 0 || tcp_async_completed != 0 || tcp_async_failed != 0 ||
                                    tcp_async_overflow != 0 || tcp_async_partial != 0 || tcp_async_coalesced != 0 || tcp_async_inflight_max != 0;
    if (tcp_async_activity)
    {
        WD_LOG_DEBUG("[client tcp_async/min] queued=%llu completed=%llu failed=%llu overflow=%llu partial=%llu coalesced=%llu inflight_max=%llu",
                     static_cast<unsigned long long>(tcp_async_queued), static_cast<unsigned long long>(tcp_async_completed),
                     static_cast<unsigned long long>(tcp_async_failed), static_cast<unsigned long long>(tcp_async_overflow),
                     static_cast<unsigned long long>(tcp_async_partial), static_cast<unsigned long long>(tcp_async_coalesced),
                     static_cast<unsigned long long>(tcp_async_inflight_max));
    }

    uint64_t timeout_ms = state.tile_reassembly_timeout_ns.load(std::memory_order_relaxed) / 1000000ull;
    const bool latency_activity = timeout_updates != 0 || timeout_ms != state.stats_log.prev_timeout_ms ||
                                  udp_gap_pressure_ms != state.stats_log.prev_udp_gap_pressure_ms || tile_assembly_samples != 0 ||
                                  tile_present_samples != 0 || input_to_present_samples != 0 || input_seq_present_samples != 0;
    if (latency_activity)
    {
        WD_LOG_DEBUG("[client latency/min] tile_assembly_avg_ms=%.2f reassembly_timeout_ms=%llu udp_gap_pressure_ms=%llu timeout_updates=%llu tile_present_avg_ms=%.2f input_to_present_avg_ms=%.2f input_seq_to_present_avg_ms=%.2f",
                     avg_ms(tile_assembly_sum_ns, tile_assembly_samples), static_cast<unsigned long long>(timeout_ms),
                     static_cast<unsigned long long>(udp_gap_pressure_ms),
                     static_cast<unsigned long long>(timeout_updates), avg_ms(tile_present_sum_ns, tile_present_samples),
                     avg_ms(input_to_present_sum_ns, input_to_present_samples), avg_ms(input_seq_present_sum_ns, input_seq_present_samples));
        state.stats_log.prev_timeout_ms = timeout_ms;
        state.stats_log.prev_udp_gap_pressure_ms = udp_gap_pressure_ms;
    }

    if (sdl_render_frames != 0 || sdl_texture_upload_samples != 0 || sdl_present_samples != 0)
    {
        WD_LOG_DEBUG("[client render/min] frames=%llu remote_frames=%llu empty_remote=%llu texture_full=%llu texture_partial=%llu dirty_rects=%llu source_rects=%llu coalesced_rects=%llu bounds_uploads=%llu upload_mpix=%.2f upload_avg_ms=%.2f upload_max_ms=%.2f present_avg_ms=%.2f present_max_ms=%.2f",
                     static_cast<unsigned long long>(sdl_render_frames),
                     static_cast<unsigned long long>(sdl_remote_frames),
                     static_cast<unsigned long long>(sdl_empty_remote_wakeups),
                     static_cast<unsigned long long>(sdl_texture_full_uploads),
                     static_cast<unsigned long long>(sdl_texture_partial_uploads),
                     static_cast<unsigned long long>(sdl_texture_dirty_rects),
                     static_cast<unsigned long long>(sdl_texture_source_dirty_rects),
                     static_cast<unsigned long long>(sdl_texture_coalesced_dirty_rects),
                     static_cast<unsigned long long>(sdl_texture_bounds_uploads),
                     static_cast<double>(sdl_texture_upload_pixels) / 1000000.0,
                     avg_ms(sdl_texture_upload_sum_ns, sdl_texture_upload_samples),
                     static_cast<double>(sdl_texture_upload_max_ns) / 1000000.0,
                     avg_ms(sdl_present_sum_ns, sdl_present_samples),
                     static_cast<double>(sdl_present_max_ns) / 1000000.0);
    }
}

void sample_client_stats(ClientState& state, bool log_stats) {
    client_reap_async_sends(state);

    const uint64_t udp_packets              = take_stat(state.stats.udp_packets_rx);
    const uint64_t udp_bytes                = take_stat(state.stats.udp_bytes_rx);
    const uint64_t udp_interarrival_samples = take_stat(state.stats.udp_interarrival_samples);
    const uint64_t udp_interarrival_sum_ns  = take_stat(state.stats.udp_interarrival_sum_ns);
    const uint64_t udp_jitter_samples       = take_stat(state.stats.udp_interarrival_jitter_samples);
    const uint64_t udp_jitter_sum_ns        = take_stat(state.stats.udp_interarrival_jitter_sum_ns);
    const uint64_t udp_interarrival_max_ns  = take_stat(state.stats.udp_interarrival_max_ns);
    const uint64_t invalid                  = take_stat(state.stats.udp_ignored_invalid);
    const uint64_t ignored_probe            = take_stat(state.stats.udp_ignored_probe);
    const uint64_t stale_session            = take_stat(state.stats.udp_ignored_stale_session);
    const uint64_t old_gen                  = take_stat(state.stats.udp_ignored_old_generation);
    const uint64_t completed                = take_stat(state.stats.udp_tiles_completed);
    const uint64_t completed_compressed     = take_stat(state.stats.udp_completed_compressed_bytes);
    const uint64_t completed_packets        = take_stat(state.stats.udp_completed_packets);
    const uint64_t partial_timeouts         = take_stat(state.stats.partial_tiles_timed_out);
    const uint64_t partial_missing_packets  = take_stat(state.stats.partial_tile_missing_packets);
    const uint64_t partial_retx_queued      = take_stat(state.stats.partial_tile_retx_queued);
    const uint64_t retx_response_samples    = take_stat(state.stats.retx_response_samples);
    const uint64_t retx_response_sum_ns     = take_stat(state.stats.retx_response_sum_ns);
    const uint64_t timeout_updates          = take_stat(state.stats.tile_reassembly_timeout_updates);
    const uint64_t summaries                = take_stat(state.stats.tcp_summaries_rx);
    const uint64_t retx                     = take_stat(state.stats.tcp_retx_requests_tx);
    const uint64_t summary_retx_queued      = take_stat(state.stats.summary_retx_tiles_queued);
    const uint64_t summary_retx_deferred    = take_stat(state.stats.summary_retx_tiles_deferred);
    const uint64_t summary_retx_throttled   = take_stat(state.stats.summary_retx_tiles_throttled);
    const uint64_t summary_retx_stale_dropped = take_stat(state.stats.summary_retx_tiles_stale_dropped);
    const uint64_t summary_retx_pressure_dropped = take_stat(state.stats.summary_retx_pressure_dropped);
    const uint64_t summary_promote_passes   = take_stat(state.stats.summary_promote_passes);
    const uint64_t summary_promote_scanned  = take_stat(state.stats.summary_promote_scanned);
    const uint64_t summary_promote_candidates = take_stat(state.stats.summary_promote_candidates);
    const uint64_t summary_to_retx_samples  = take_stat(state.stats.summary_to_retx_samples);
    const uint64_t summary_to_retx_sum_ns   = take_stat(state.stats.summary_to_retx_sum_ns);
    const uint64_t keys                     = take_stat(state.stats.tcp_keyboard_tx);
    const uint64_t pointer                  = take_stat(state.stats.tcp_pointer_tx);
    const uint64_t input_events             = take_stat(state.stats.tcp_input_events_tx);
    const uint64_t input_channel_events     = take_stat(state.stats.tcp_input_channel_tx);
    const uint64_t input_fallback_events    = take_stat(state.stats.tcp_input_channel_fallback_tx);
    const uint64_t selection_channel_events = take_stat(state.stats.tcp_selection_channel_tx);
    const uint64_t selection_fallback_events = take_stat(state.stats.tcp_selection_channel_fallback_tx);
    const uint64_t tcp_async_queued         = take_stat(state.stats.tcp_async_queued);
    const uint64_t tcp_async_completed      = take_stat(state.stats.tcp_async_completed);
    const uint64_t tcp_async_failed         = take_stat(state.stats.tcp_async_failed);
    const uint64_t tcp_async_overflow       = take_stat(state.stats.tcp_async_overflow);
    const uint64_t tcp_async_partial        = take_stat(state.stats.tcp_async_partial);
    const uint64_t tcp_async_coalesced      = take_stat(state.stats.tcp_async_coalesced);
    const uint64_t tcp_async_inflight_max   = take_stat(state.stats.tcp_async_inflight_max);
    client_reap_async_udp_receives(state);
    const uint64_t udp_async_posted         = take_stat(state.stats.udp_async_posted);
    const uint64_t udp_async_completed      = take_stat(state.stats.udp_async_completed);
    const uint64_t udp_async_failed         = take_stat(state.stats.udp_async_failed);
    const uint64_t udp_async_submit_failed  = take_stat(state.stats.udp_async_submit_failed);
    const uint64_t udp_async_cancels        = take_stat(state.stats.udp_async_cancels);
    const uint64_t udp_async_inflight_max   = take_stat(state.stats.udp_async_inflight_max);
    const uint64_t tile_assembly_samples    = take_stat(state.stats.tile_assembly_samples);
    const uint64_t tile_assembly_sum_ns     = take_stat(state.stats.tile_assembly_sum_ns);
    const uint64_t tile_present_samples     = take_stat(state.stats.tile_present_latency_samples);
    const uint64_t tile_present_sum_ns      = take_stat(state.stats.tile_present_latency_sum_ns);
    const uint64_t input_to_present_samples = take_stat(state.stats.input_to_present_latency_samples);
    const uint64_t input_to_present_sum_ns  = take_stat(state.stats.input_to_present_latency_sum_ns);
    const uint64_t input_seq_present_samples = take_stat(state.stats.input_sequence_present_latency_samples);
    const uint64_t input_seq_present_sum_ns  = take_stat(state.stats.input_sequence_present_latency_sum_ns);
    const uint64_t sdl_render_frames          = take_stat(state.stats.sdl_render_frames);
    const uint64_t sdl_remote_frames          = take_stat(state.stats.sdl_remote_frames);
    const uint64_t sdl_empty_remote_wakeups   = take_stat(state.stats.sdl_empty_remote_wakeups);
    const uint64_t sdl_texture_full_uploads   = take_stat(state.stats.sdl_texture_full_uploads);
    const uint64_t sdl_texture_partial_uploads = take_stat(state.stats.sdl_texture_partial_uploads);
    const uint64_t sdl_texture_dirty_rects    = take_stat(state.stats.sdl_texture_dirty_rects);
    const uint64_t sdl_texture_source_dirty_rects = take_stat(state.stats.sdl_texture_source_dirty_rects);
    const uint64_t sdl_texture_coalesced_dirty_rects = take_stat(state.stats.sdl_texture_coalesced_dirty_rects);
    const uint64_t sdl_texture_bounds_uploads = take_stat(state.stats.sdl_texture_bounds_uploads);
    const uint64_t sdl_texture_upload_pixels  = take_stat(state.stats.sdl_texture_upload_pixels);
    const uint64_t sdl_texture_upload_samples = take_stat(state.stats.sdl_texture_upload_samples);
    const uint64_t sdl_texture_upload_sum_ns  = take_stat(state.stats.sdl_texture_upload_sum_ns);
    const uint64_t sdl_texture_upload_max_ns  = take_stat(state.stats.sdl_texture_upload_max_ns);
    const uint64_t sdl_present_samples        = take_stat(state.stats.sdl_present_samples);
    const uint64_t sdl_present_sum_ns         = take_stat(state.stats.sdl_present_sum_ns);
    const uint64_t sdl_present_max_ns         = take_stat(state.stats.sdl_present_max_ns);

    ClientStatsSnapshot sample{};
    sample.udp_packets = udp_packets;
    sample.udp_bytes = udp_bytes;
    sample.udp_interarrival_samples = udp_interarrival_samples;
    sample.udp_interarrival_sum_ns = udp_interarrival_sum_ns;
    sample.udp_jitter_samples = udp_jitter_samples;
    sample.udp_jitter_sum_ns = udp_jitter_sum_ns;
    sample.udp_interarrival_max_ns = udp_interarrival_max_ns;
    sample.invalid = invalid;
    sample.ignored_probe = ignored_probe;
    sample.stale_session = stale_session;
    sample.old_gen = old_gen;
    sample.completed = completed;
    sample.completed_compressed = completed_compressed;
    sample.completed_packets = completed_packets;
    sample.partial_timeouts = partial_timeouts;
    sample.partial_missing_packets = partial_missing_packets;
    sample.partial_retx_queued = partial_retx_queued;
    sample.retx_response_samples = retx_response_samples;
    sample.retx_response_sum_ns = retx_response_sum_ns;
    sample.timeout_updates = timeout_updates;
    sample.summaries = summaries;
    sample.retx = retx;
    sample.summary_retx_queued = summary_retx_queued;
    sample.summary_retx_deferred = summary_retx_deferred;
    sample.summary_retx_throttled = summary_retx_throttled;
    sample.summary_retx_stale_dropped = summary_retx_stale_dropped;
    sample.summary_retx_pressure_dropped = summary_retx_pressure_dropped;
    sample.summary_promote_passes = summary_promote_passes;
    sample.summary_promote_scanned = summary_promote_scanned;
    sample.summary_promote_candidates = summary_promote_candidates;
    sample.summary_to_retx_samples = summary_to_retx_samples;
    sample.summary_to_retx_sum_ns = summary_to_retx_sum_ns;
    sample.keys = keys;
    sample.pointer = pointer;
    sample.input_events = input_events;
    sample.input_channel_events = input_channel_events;
    sample.input_fallback_events = input_fallback_events;
    sample.selection_channel_events = selection_channel_events;
    sample.selection_fallback_events = selection_fallback_events;
    sample.tcp_async_queued = tcp_async_queued;
    sample.tcp_async_completed = tcp_async_completed;
    sample.tcp_async_failed = tcp_async_failed;
    sample.tcp_async_overflow = tcp_async_overflow;
    sample.tcp_async_partial = tcp_async_partial;
    sample.tcp_async_coalesced = tcp_async_coalesced;
    sample.tcp_async_inflight_max = tcp_async_inflight_max;
    sample.udp_async_posted = udp_async_posted;
    sample.udp_async_completed = udp_async_completed;
    sample.udp_async_failed = udp_async_failed;
    sample.udp_async_submit_failed = udp_async_submit_failed;
    sample.udp_async_cancels = udp_async_cancels;
    sample.udp_async_inflight_max = udp_async_inflight_max;
    sample.tile_assembly_samples = tile_assembly_samples;
    sample.tile_assembly_sum_ns = tile_assembly_sum_ns;
    sample.tile_present_samples = tile_present_samples;
    sample.tile_present_sum_ns = tile_present_sum_ns;
    sample.input_to_present_samples = input_to_present_samples;
    sample.input_to_present_sum_ns = input_to_present_sum_ns;
    sample.input_seq_present_samples = input_seq_present_samples;
    sample.input_seq_present_sum_ns = input_seq_present_sum_ns;
    sample.sdl_render_frames = sdl_render_frames;
    sample.sdl_remote_frames = sdl_remote_frames;
    sample.sdl_empty_remote_wakeups = sdl_empty_remote_wakeups;
    sample.sdl_texture_full_uploads = sdl_texture_full_uploads;
    sample.sdl_texture_partial_uploads = sdl_texture_partial_uploads;
    sample.sdl_texture_dirty_rects = sdl_texture_dirty_rects;
    sample.sdl_texture_source_dirty_rects = sdl_texture_source_dirty_rects;
    sample.sdl_texture_coalesced_dirty_rects = sdl_texture_coalesced_dirty_rects;
    sample.sdl_texture_bounds_uploads = sdl_texture_bounds_uploads;
    sample.sdl_texture_upload_pixels = sdl_texture_upload_pixels;
    sample.sdl_texture_upload_samples = sdl_texture_upload_samples;
    sample.sdl_texture_upload_sum_ns = sdl_texture_upload_sum_ns;
    sample.sdl_texture_upload_max_ns = sdl_texture_upload_max_ns;
    sample.sdl_present_samples = sdl_present_samples;
    sample.sdl_present_sum_ns = sdl_present_sum_ns;
    sample.sdl_present_max_ns = sdl_present_max_ns;
    client_stats_accumulate(state.stats_log.totals, sample);

    const bool feedback_activity = udp_packets != 0 || udp_bytes != 0 || completed != 0 || partial_timeouts != 0 ||
                                   invalid != 0 || old_gen != 0 || retx != 0 || udp_interarrival_samples != 0;

    if (feedback_activity)
    {
        wd_client_stats_payload feedback{};
        {
            std::lock_guard<std::mutex> lock(state.config_mutex);
            feedback.session_id = state.config.session_id;
        }
        if (state.render_feedback_visible.load(std::memory_order_relaxed))
        {
            feedback.flags |= WD_CLIENT_STATS_RENDER_VISIBLE;
        }
        feedback.udp_packets_rx             = udp_packets;
        feedback.udp_bytes_rx               = udp_bytes;
        feedback.udp_tiles_completed        = completed;
        feedback.udp_completed_packets      = completed_packets;
        feedback.partial_tiles_timed_out    = partial_timeouts;
        feedback.udp_ignored_old_generation = old_gen;
        feedback.retx_requests_tx           = retx;
        feedback.udp_interarrival_samples   = udp_interarrival_samples;
        feedback.udp_interarrival_sum_ns    = udp_interarrival_sum_ns;
        feedback.udp_interarrival_jitter_samples = udp_jitter_samples;
        feedback.udp_interarrival_jitter_sum_ns  = udp_jitter_sum_ns;
        feedback.udp_interarrival_max_ns    = udp_interarrival_max_ns;
        feedback.render_frames              = sdl_render_frames;
        feedback.present_samples            = sdl_present_samples;
        feedback.present_sum_ns             = sdl_present_sum_ns;
        feedback.present_max_ns             = sdl_present_max_ns;
        feedback.input_present_samples      = input_seq_present_samples;
        feedback.input_present_sum_ns       = input_seq_present_sum_ns;
        if (feedback.session_id != 0)
        {
            client_send_stats(state, feedback);
        }
    }

    update_udp_gap_pressure(state, udp_interarrival_max_ns, udp_interarrival_samples, udp_packets);
    if (!log_stats)
    {
        return;
    }

    const ClientStatsSnapshot logged = state.stats_log.totals;
    state.stats_log.totals = {};
    log_client_stats_snapshot(state, logged);
}

bool blit_tile_xrgb8888(ClientState& state, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                      const std::vector<uint8_t>& tile_bytes, ClientDirtyRect& dirty_rect) {
    const uint16_t tiles_x = wd_tiles_for_width_with_tile(state.config.width, tile_width);
    const uint16_t tiles_y = wd_tiles_for_height_with_tile(state.config.height, tile_height);
    const uint32_t total_tiles = static_cast<uint32_t>(tiles_x) * static_cast<uint32_t>(tiles_y);
    if (tile_width == 0 || tile_height == 0 || tiles_x == 0 || tiles_y == 0 || tile_id >= total_tiles)
    {
        return false;
    }

    const uint32_t tile_x = tile_id % tiles_x;
    const uint32_t tile_y = tile_id / tiles_x;
    const uint32_t dst_x  = tile_x * tile_width;
    const uint32_t dst_y  = tile_y * tile_height;
    const size_t   expected_size = static_cast<size_t>(tile_width) * static_cast<size_t>(tile_height) * WD_BYTES_PER_PIXEL;

    if (tile_bytes.size() < expected_size || dst_x >= state.config.width || dst_y >= state.config.height)
    {
        return false;
    }

    const uint32_t visible_width  = std::min<uint32_t>(tile_width, state.config.width - dst_x);
    const uint32_t visible_height = std::min<uint32_t>(tile_height, state.config.height - dst_y);

    dirty_rect.x = static_cast<uint16_t>(dst_x);
    dirty_rect.y = static_cast<uint16_t>(dst_y);
    dirty_rect.w = static_cast<uint16_t>(visible_width);
    dirty_rect.h = static_cast<uint16_t>(visible_height);

    for (uint32_t y = 0; y < visible_height; ++y)
    {
        const uint8_t* src = tile_bytes.data() + static_cast<size_t>(y) * tile_width * WD_BYTES_PER_PIXEL;
        uint32_t*      dst = state.framebuffer.data() + static_cast<size_t>(dst_y + y) * state.config.width + dst_x;

        std::memcpy(dst, src, static_cast<size_t>(visible_width) * WD_BYTES_PER_PIXEL);
    }

    return true;
}

void clear_completed_repair_tracking_locked(ClientState& state, uint32_t base_id) {
    if (base_id >= state.displayed_generation.size())
    {
        return;
    }

    const uint64_t displayed = state.displayed_generation[base_id];
    if (base_id < state.retx_queued_generation.size() && state.retx_queued_generation[base_id] != 0 &&
        state.retx_queued_generation[base_id] <= displayed)
    {
        state.retx_queued_generation[base_id] = 0;
    }
    if (base_id < state.retx_inflight_generation.size() && state.retx_inflight_generation[base_id] != 0 &&
        state.retx_inflight_generation[base_id] <= displayed)
    {
        state.retx_inflight_generation[base_id] = 0;
        if (base_id < state.retx_inflight_since_ns.size())
        {
            state.retx_inflight_since_ns[base_id] = 0;
        }
    }
    if (base_id < state.retx_last_requested_generation.size() && state.retx_last_requested_generation[base_id] != 0 &&
        state.retx_last_requested_generation[base_id] <= displayed)
    {
        state.retx_last_requested_generation[base_id] = 0;
        if (base_id < state.retx_last_request_ns.size())
        {
            state.retx_last_request_ns[base_id] = 0;
        }
    }
    if (base_id < state.retx_summary_pending_generation.size() && state.retx_summary_pending_generation[base_id] != 0 &&
        state.retx_summary_pending_generation[base_id] <= displayed)
    {
        if (state.retx_summary_pending_count > 0)
        {
            state.retx_summary_pending_count--;
        }
        state.retx_summary_pending_generation[base_id] = 0;
        if (base_id < state.retx_summary_pending_since_ns.size())
        {
            state.retx_summary_pending_since_ns[base_id] = 0;
        }
        state.stats.summary_retx_tiles_stale_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void mark_completed_base_generations(ClientState& state, const CompletedTile& completed) {
    if (state.config.tile_width == 0 || state.config.tile_height == 0 || completed.tile_width == 0 || completed.tile_height == 0)
    {
        return;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(state.config.width, completed.tile_width);
    if (tiles_x == 0)
    {
        return;
    }

    const uint32_t x = wd_tile_start_x_for_tile(completed.tile_id, tiles_x, completed.tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(completed.tile_id, tiles_x, completed.tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(state.config.width, completed.tile_id, tiles_x, completed.tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(state.config.height, completed.tile_id, tiles_x, completed.tile_height);
    if (w == 0 || h == 0)
    {
        return;
    }

    uint32_t bx0 = x / state.config.tile_width;
    uint32_t by0 = y / state.config.tile_height;
    uint32_t bx1 = (x + w - 1u) / state.config.tile_width;
    uint32_t by1 = (y + h - 1u) / state.config.tile_height;
    if (bx1 >= state.config.tiles_x)
    {
        bx1 = static_cast<uint32_t>(state.config.tiles_x) - 1u;
    }
    if (by1 >= state.config.tiles_y)
    {
        by1 = static_cast<uint32_t>(state.config.tiles_y) - 1u;
    }

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            const uint32_t base_id = by * static_cast<uint32_t>(state.config.tiles_x) + bx;
            if (base_id < state.displayed_generation.size() && completed.generation > state.displayed_generation[base_id])
            {
                state.displayed_generation[base_id] = completed.generation;
            }
            clear_completed_repair_tracking_locked(state, base_id);
        }
    }
}

bool client_has_pending_server_config(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.config_mutex);
    return state.pending_config_valid;
}

bool process_udp_datagram(ClientState& state, TileReassembler& reassembler, const uint8_t* packet, size_t packet_size) {
    if (!packet || packet_size < WD_UDP_TILE_HEADER_MIN_SIZE)
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    wd_udp_tile_packet_decoded udp_header{};
    if (!wd_udp_tile_packet_decode(packet, packet_size, &udp_header))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (udp_header.tile_id == WD_UDP_TILE_ID_MTU_PROBE || udp_header.tile_id == WD_UDP_TILE_ID_THROUGHPUT_PROBE)
    {
        state.stats.udp_ignored_probe.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    uint8_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id = state.config.session_id;
    }

    if (session_id == 0 || udp_header.session_id != session_id)
    {
        state.stats.udp_ignored_stale_session.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const uint64_t packet_rx_ns = wd_now_ns();
    const uint64_t prev_rx_ns = state.stats.last_udp_packet_rx_ns.exchange(packet_rx_ns, std::memory_order_relaxed);
    if (prev_rx_ns != 0 && packet_rx_ns >= prev_rx_ns)
    {
        const uint64_t interarrival_ns = packet_rx_ns - prev_rx_ns;
        state.stats.udp_interarrival_samples.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_interarrival_sum_ns.fetch_add(interarrival_ns, std::memory_order_relaxed);
        record_atomic_max(state.stats.udp_interarrival_max_ns, interarrival_ns);

        const uint64_t prev_interarrival_ns =
            state.stats.last_udp_interarrival_ns.exchange(interarrival_ns, std::memory_order_relaxed);
        if (prev_interarrival_ns != 0)
        {
            const uint64_t jitter_ns = interarrival_ns > prev_interarrival_ns ? interarrival_ns - prev_interarrival_ns
                                                                              : prev_interarrival_ns - interarrival_ns;
            state.stats.udp_interarrival_jitter_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.udp_interarrival_jitter_sum_ns.fetch_add(jitter_ns, std::memory_order_relaxed);
        }
    }

    state.stats.udp_packets_rx.fetch_add(1, std::memory_order_relaxed);
    state.stats.udp_bytes_rx.fetch_add(static_cast<uint64_t>(packet_size), std::memory_order_relaxed);

    CompletedTile completed = reassembler.process_udp_packet(state, packet, packet_size);

    if (!completed.valid)
    {
        return true;
    }

    ClientDirtyRect dirty_rect{};
    {
        std::lock_guard<std::mutex> framebuffer_lock(state.framebuffer_mutex);
        if (!blit_tile_xrgb8888(state, completed.tile_id, completed.tile_width, completed.tile_height, completed.tile_bytes, dirty_rect))
        {
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    {
        std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
        state.pending_dirty_rects.push_back(dirty_rect);
        state.pending_dirty_rect_count.store(state.pending_dirty_rects.size(), std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);
        mark_completed_base_generations(state, completed);
    }

    if (completed.first_packet_ns != 0 && completed.completed_timestamp_ns >= completed.first_packet_ns)
    {
        state.stats.tile_assembly_samples.fetch_add(1, std::memory_order_relaxed);
        state.stats.tile_assembly_sum_ns.fetch_add(completed.completed_timestamp_ns - completed.first_packet_ns,
                                                   std::memory_order_relaxed);
    }

    if (completed.completed_timestamp_ns != 0)
    {
        std::lock_guard<std::mutex> present_lock(state.present_mutex);
        state.pending_present_tile_timestamps.push_back(completed.completed_timestamp_ns);
        state.pending_present_input_sequences.push_back(completed.input_sequence);
    }

    state.stats.udp_completed_compressed_bytes.fetch_add(completed.compressed_size, std::memory_order_relaxed);
    state.stats.udp_completed_packets.fetch_add(completed.packet_count, std::memory_order_relaxed);
    state.stats.udp_tiles_completed.fetch_add(1, std::memory_order_relaxed);
    state.frame_dirty.store(true, std::memory_order_release);
    return true;
}

struct AsyncUdpDrainContext {
    ClientState*      state       = nullptr;
    TileReassembler* reassembler = nullptr;
};

bool handle_async_udp_packet(void* userdata, const uint8_t* packet, size_t packet_size) {
    auto* ctx = static_cast<AsyncUdpDrainContext*>(userdata);
    if (!ctx || !ctx->state || !ctx->reassembler)
    {
        return false;
    }
    if (client_has_pending_server_config(*ctx->state))
    {
        return true;
    }
    return process_udp_datagram(*ctx->state, *ctx->reassembler, packet, packet_size);
}

bool drain_udp_sync_locked(ClientState& state, TileReassembler& reassembler) {
    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    const size_t recvbuf_size = WD_UDP_TILE_HEADER_MAX_SIZE + udp_payload_target + 512;

    if (state.udp_recv_buffer.size() < recvbuf_size)
    {
        state.udp_recv_buffer.assign(recvbuf_size, 0);
    }

    for (;;)
    {
        if (client_has_pending_server_config(state))
        {
            return true;
        }

        ssize_t n = ::recv(state.udp_fd, state.udp_recv_buffer.data(), state.udp_recv_buffer.size(), 0);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return true;
            }

            if (errno == EINTR)
            {
                continue;
            }

            WD_LOG_ERROR("recv UDP failed: %s", std::strerror(errno));
            return false;
        }

        if (n == 0)
        {
            return true;
        }

        if (!process_udp_datagram(state, reassembler, state.udp_recv_buffer.data(), static_cast<size_t>(n)))
        {
            return false;
        }
    }
}

bool drain_udp(ClientState& state, TileReassembler& reassembler) {
    std::lock_guard<std::mutex> processing_lock(state.udp_processing_mutex);

    if (state.udp_receiver)
    {
        AsyncUdpDrainContext ctx{&state, &reassembler};
        const bool ok = client_async_udp_receiver_drain(state.udp_receiver, &ctx, handle_async_udp_packet, 8192);
        client_reap_async_udp_receives(state);
        if (ok)
        {
            return true;
        }

        if (!client_disable_async_udp_receiver(state))
        {
            return false;
        }
    }

    return drain_udp_sync_locked(state, reassembler);
}


void udp_reader_main(ClientState* state) {
    TileReassembler reassembler;
    uint64_t observed_config_generation = state->client_config_generation.load(std::memory_order_acquire);

    while (state->running.load(std::memory_order_relaxed))
    {
        const uint64_t current_generation = state->client_config_generation.load(std::memory_order_acquire);
        if (current_generation != observed_config_generation)
        {
            reassembler.reset();
            observed_config_generation = current_generation;
        }

        if (client_has_pending_server_config(*state))
        {
            SDL_Delay(1);
            continue;
        }

        if (!drain_udp(*state, reassembler))
        {
            state->running.store(false, std::memory_order_relaxed);
            break;
        }

        {
            std::lock_guard<std::mutex> processing_lock(state->udp_processing_mutex);
            reassembler.expire_stale_entries(*state);
        }
        const uint64_t summary_promote_now_ns = wd_now_ns();
        if (state->next_summary_promote_ns == 0 || summary_promote_now_ns >= state->next_summary_promote_ns)
        {
            client_promote_deferred_summary_retransmits(*state);
            state->next_summary_promote_ns = summary_promote_now_ns + WD_CLIENT_SUMMARY_PROMOTE_INTERVAL_NS;
        }

        if (!client_flush_retransmit_requests(*state))
        {
            WD_LOG_ERROR("failed to send retransmit request");
            state->running.store(false, std::memory_order_relaxed);
            break;
        }

        if (state->pending_dirty_rect_count.load(std::memory_order_acquire) == 0)
        {
            SDL_Delay(1);
        }
    }
}

uint16_t sdl_button_to_linux_button(uint8_t button) {
    switch (button)
    {
    case SDL_BUTTON_LEFT:
        return WD_BTN_LEFT;
    case SDL_BUTTON_RIGHT:
        return WD_BTN_RIGHT;
    case SDL_BUTTON_MIDDLE:
        return WD_BTN_MIDDLE;
    case SDL_BUTTON_X1:
        return WD_BTN_SIDE;
    case SDL_BUTTON_X2:
        return WD_BTN_EXTRA;
    default:
        return 0;
    }
}

uint16_t map_mouse_coord_x(float x) {
    if (!g_client_config || g_client_config->width == 0)
    {
        return 0;
    }

    if (x <= g_content_rect.x || g_content_rect.w <= 0)
    {
        return 0;
    }

    if (x >= g_content_rect.x + g_content_rect.w)
    {
        return static_cast<uint16_t>(g_client_config->width - 1);
    }

    const float    local_x = x - g_content_rect.x;
    const uint32_t mapped =
        static_cast<uint32_t>((static_cast<double>(local_x) * g_client_config->width) / g_content_rect.w);

    if (mapped >= g_client_config->width)
    {
        return static_cast<uint16_t>(g_client_config->width - 1);
    }

    return static_cast<uint16_t>(mapped);
}

uint16_t map_mouse_coord_y(float y) {
    if (!g_client_config || g_client_config->height == 0)
    {
        return 0;
    }

    if (y <= g_content_rect.y || g_content_rect.h <= 0)
    {
        return 0;
    }

    if (y >= g_content_rect.y + g_content_rect.h)
    {
        return static_cast<uint16_t>(g_client_config->height - 1);
    }

    const float    local_y = y - g_content_rect.y;
    const uint32_t mapped =
        static_cast<uint32_t>((static_cast<double>(local_y) * g_client_config->height) / g_content_rect.h);

    if (mapped >= g_client_config->height)
    {
        return static_cast<uint16_t>(g_client_config->height - 1);
    }

    return static_cast<uint16_t>(mapped);
}

void update_window_size(SDL_Window* window) {
    int width  = 1;
    int height = 1;

    if (!SDL_GetWindowSize(window, &width, &height))
    {
        log_sdl_warning("SDL_GetWindowSize");
    }

    if (width < 1)
    {
        width = 1;
    }

    if (height < 1)
    {
        height = 1;
    }

    g_window_width  = width;
    g_window_height = height;

    if (!g_client_config || g_client_config->width == 0 || g_client_config->height == 0)
    {
        g_content_rect = SDL_FRect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        return;
    }

    const uint64_t width_limited_height = (static_cast<uint64_t>(width) * g_client_config->height) / g_client_config->width;

    int content_width  = width;
    int content_height = height;

    if (width_limited_height <= static_cast<uint64_t>(height))
    {
        content_height = static_cast<int>(width_limited_height);
    }
    else
    {
        content_width = static_cast<int>((static_cast<uint64_t>(height) * g_client_config->width) / g_client_config->height);
    }

    if (content_width < 1)
    {
        content_width = 1;
    }

    if (content_height < 1)
    {
        content_height = 1;
    }

    g_content_rect = SDL_FRect{
        static_cast<float>((width - content_width) / 2),
        static_cast<float>((height - content_height) / 2),
        static_cast<float>(content_width),
        static_cast<float>(content_height),
    };
}

bool client_config_dimensions_valid(const wd_server_config_payload& config) {
    if (config.width == 0 || config.height == 0 || config.width > WD_CLIENT_MAX_DIMENSION || config.height > WD_CLIENT_MAX_DIMENSION)
    {
        return false;
    }

    const uint64_t pixels = static_cast<uint64_t>(config.width) * config.height;
    const uint64_t bytes  = pixels * WD_BYTES_PER_PIXEL;

    return bytes <= WD_CLIENT_MAX_FRAMEBUFFER_BYTES;
}

SDL_Texture* create_frame_texture(SDL_Renderer* renderer, uint32_t width, uint32_t height) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             static_cast<int>(width), static_cast<int>(height));

    if (texture)
    {
        if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST))
        {
            log_sdl_warning("SDL_SetTextureScaleMode");
        }
    }

    return texture;
}

bool apply_pending_server_config(ClientState& state, SDL_Window* window, SDL_Renderer* renderer, SDL_Texture*& texture) {
    wd_server_config_payload config{};

    {
        std::lock_guard<std::mutex> lock(state.config_mutex);

        if (!state.pending_config_valid)
        {
            return false;
        }

        config                     = state.pending_config;
        state.pending_config_valid = false;
    }

    if (!client_config_dimensions_valid(config))
    {
        WD_LOG_ERROR("refusing oversized server display resize: %ux%u", config.width, config.height);
        return false;
    }

    if (config.session_id == state.config.session_id && config.width == state.config.width && config.height == state.config.height &&
        config.tile_width == state.config.tile_width && config.tile_height == state.config.tile_height &&
        config.tiles_x == state.config.tiles_x && config.tiles_y == state.config.tiles_y && config.total_tiles == state.config.total_tiles)
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        state.config = config;
        return false;
    }

    WD_LOG_INFO("server config updated: session=%u display=%ux%u tile=%ux%u grid=%ux%u total=%u", config.session_id,
                config.width, config.height, config.tile_width, config.tile_height, config.tiles_x, config.tiles_y, config.total_tiles);

    std::vector<uint32_t> new_framebuffer(static_cast<size_t>(config.width) * config.height, 0xff202020u);
    std::vector<uint64_t> new_displayed_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_queued_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_last_requested_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_last_request_ns(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_inflight_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_inflight_since_ns(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_summary_pending_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_summary_pending_since_ns(config.total_tiles, 0);
    std::vector<uint8_t>  new_udp_recv_buffer(WD_UDP_TILE_HEADER_MAX_SIZE + config.udp_payload_target + 512, 0);

    SDL_Texture* new_texture =
        create_frame_texture(renderer, config.width, config.height);

    if (!new_texture)
    {
        WD_LOG_ERROR("SDL_CreateTexture after resize failed: %s", SDL_GetError());
        state.running.store(false, std::memory_order_relaxed);
        return false;
    }

    {
        std::lock_guard<std::mutex> processing_lock(state.udp_processing_mutex);
        std::lock_guard<std::mutex> config_lock(state.config_mutex);
        std::lock_guard<std::mutex> framebuffer_lock(state.framebuffer_mutex);
        std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

        state.config               = config;
        state.framebuffer          = std::move(new_framebuffer);
        state.pending_dirty_rects.clear();
        state.pending_dirty_rect_count.store(0, std::memory_order_release);
        state.displayed_generation = std::move(new_displayed_generation);
        state.retx_queue.clear();
        state.retx_queued_generation          = std::move(new_retx_queued_generation);
        state.retx_last_requested_generation = std::move(new_retx_last_requested_generation);
        state.retx_last_request_ns           = std::move(new_retx_last_request_ns);
        state.retx_inflight_generation       = std::move(new_retx_inflight_generation);
        state.retx_inflight_since_ns         = std::move(new_retx_inflight_since_ns);
        state.retx_summary_pending_generation = std::move(new_retx_summary_pending_generation);
        state.retx_summary_pending_since_ns   = std::move(new_retx_summary_pending_since_ns);
        state.retx_summary_pending_count = 0;
        state.next_summary_promote_ns = 0;
        state.summary_large_repair_not_before_ns = 0;
        state.summary_repair_loss_signal_until_ns.store(0, std::memory_order_relaxed);
        state.udp_recv_buffer                = std::move(new_udp_recv_buffer);
        state.client_config_generation.fetch_add(1, std::memory_order_release);
        state.frame_dirty.store(true, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> present_lock(state.present_mutex);
        state.pending_present_tile_timestamps.clear();
        state.pending_present_input_sequences.clear();
    }

    SDL_DestroyTexture(texture);
    texture = new_texture;

    g_client_config = &state.config;
    if (!SDL_SetWindowMinimumSize(window, WD_CLIENT_MIN_WINDOW_WIDTH, WD_CLIENT_MIN_WINDOW_HEIGHT))
    {
        log_sdl_warning("SDL_SetWindowMinimumSize");
    }
    update_window_size(window);

    return true;
}

uint64_t dirty_rect_pixel_count(const std::vector<ClientDirtyRect>& rects) {
    uint64_t pixels = 0;
    for (const ClientDirtyRect& rect : rects)
    {
        pixels += static_cast<uint64_t>(rect.w) * static_cast<uint64_t>(rect.h);
    }
    return pixels;
}

bool should_upload_full_texture(ClientState& state, const std::vector<ClientDirtyRect>& rects) {
    if (rects.empty())
    {
        return true;
    }

    if (rects.size() >= WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_THRESHOLD)
    {
        return true;
    }

    const uint64_t frame_pixels = static_cast<uint64_t>(state.config.width) * static_cast<uint64_t>(state.config.height);
    if (frame_pixels == 0)
    {
        return true;
    }

    const uint64_t dirty_pixels = dirty_rect_pixel_count(rects);
    return dirty_pixels * 100ull >= frame_pixels * static_cast<uint64_t>(WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_PERCENT);
}
bool clamp_dirty_rect(const ClientDirtyRect& in, uint32_t frame_width, uint32_t frame_height, ClientDirtyRect& out) {
    if (in.w == 0 || in.h == 0 || in.x >= frame_width || in.y >= frame_height)
    {
        return false;
    }

    const uint32_t visible_width  = std::min<uint32_t>(in.w, frame_width - in.x);
    const uint32_t visible_height = std::min<uint32_t>(in.h, frame_height - in.y);
    if (visible_width == 0 || visible_height == 0 || visible_width > UINT16_MAX || visible_height > UINT16_MAX)
    {
        return false;
    }

    out.x = in.x;
    out.y = in.y;
    out.w = static_cast<uint16_t>(visible_width);
    out.h = static_cast<uint16_t>(visible_height);
    return true;
}

ClientDirtyRect bounding_dirty_rect(const std::vector<ClientDirtyRect>& rects) {
    uint32_t min_x = UINT32_MAX;
    uint32_t min_y = UINT32_MAX;
    uint32_t max_x = 0;
    uint32_t max_y = 0;

    for (const ClientDirtyRect& rect : rects)
    {
        min_x = std::min<uint32_t>(min_x, rect.x);
        min_y = std::min<uint32_t>(min_y, rect.y);
        max_x = std::max<uint32_t>(max_x, static_cast<uint32_t>(rect.x) + rect.w);
        max_y = std::max<uint32_t>(max_y, static_cast<uint32_t>(rect.y) + rect.h);
    }

    ClientDirtyRect out{};
    if (min_x == UINT32_MAX || min_y == UINT32_MAX || max_x <= min_x || max_y <= min_y)
    {
        return out;
    }

    out.x = static_cast<uint16_t>(min_x);
    out.y = static_cast<uint16_t>(min_y);
    out.w = static_cast<uint16_t>(std::min<uint32_t>(max_x - min_x, UINT16_MAX));
    out.h = static_cast<uint16_t>(std::min<uint32_t>(max_y - min_y, UINT16_MAX));
    return out;
}

void coalesce_dirty_texture_rects(std::vector<ClientDirtyRect>& rects, uint32_t frame_width, uint32_t frame_height,
                                  bool& used_bounds_upload) {
    used_bounds_upload = false;
    if (rects.empty())
    {
        return;
    }

    std::vector<ClientDirtyRect> clamped;
    clamped.reserve(rects.size());
    for (const ClientDirtyRect& rect : rects)
    {
        ClientDirtyRect visible{};
        if (clamp_dirty_rect(rect, frame_width, frame_height, visible))
        {
            clamped.push_back(visible);
        }
    }

    if (clamped.empty())
    {
        rects.clear();
        return;
    }

    std::sort(clamped.begin(), clamped.end(), [](const ClientDirtyRect& a, const ClientDirtyRect& b) {
        if (a.y != b.y)
        {
            return a.y < b.y;
        }
        if (a.h != b.h)
        {
            return a.h < b.h;
        }
        return a.x < b.x;
    });

    rects.clear();
    rects.reserve(clamped.size());
    for (const ClientDirtyRect& rect : clamped)
    {
        if (!rects.empty())
        {
            ClientDirtyRect& prev = rects.back();
            const uint32_t prev_end_x = static_cast<uint32_t>(prev.x) + prev.w;
            const uint32_t rect_end_x = static_cast<uint32_t>(rect.x) + rect.w;
            if (prev.y == rect.y && prev.h == rect.h && rect.x <= prev_end_x)
            {
                prev.w = static_cast<uint16_t>(std::min<uint32_t>(std::max(prev_end_x, rect_end_x) - prev.x, UINT16_MAX));
                continue;
            }
        }
        rects.push_back(rect);
    }

    if (rects.size() > WD_CLIENT_DIRTY_RECT_BOUNDS_UPLOAD_THRESHOLD)
    {
        const ClientDirtyRect bounds = bounding_dirty_rect(rects);
        if (bounds.w != 0 && bounds.h != 0)
        {
            rects.assign(1, bounds);
            used_bounds_upload = true;
        }
    }
}


void record_texture_upload_stats(ClientState& state, uint64_t started_ns, uint64_t pixels) {
    const uint64_t elapsed_ns = wd_now_ns() - started_ns;
    state.stats.sdl_texture_upload_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_texture_upload_sum_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    state.stats.sdl_texture_upload_pixels.fetch_add(pixels, std::memory_order_relaxed);
    record_atomic_max(state.stats.sdl_texture_upload_max_ns, elapsed_ns);
}

bool upload_full_texture(ClientState& state, SDL_Texture* texture, uint32_t frame_width, uint32_t frame_height) {
    const uint64_t started_ns = wd_now_ns();
    bool ok = false;
    {
        std::lock_guard<std::mutex> framebuffer_lock(state.framebuffer_mutex);
        const size_t expected_pixels = static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height);
        ok = frame_width != 0 && frame_height != 0 && state.framebuffer.size() >= expected_pixels &&
             SDL_UpdateTexture(texture, nullptr, state.framebuffer.data(), static_cast<int>(frame_width * WD_BYTES_PER_PIXEL));
    }

    if (ok)
    {
        state.stats.sdl_texture_full_uploads.fetch_add(1, std::memory_order_relaxed);
        record_texture_upload_stats(state, started_ns, static_cast<uint64_t>(frame_width) * static_cast<uint64_t>(frame_height));
    }

    return ok;
}

bool upload_dirty_texture_rects(ClientState& state, SDL_Texture* texture, const std::vector<ClientDirtyRect>& rects,
                                uint32_t frame_width, uint32_t frame_height) {
    const uint64_t started_ns = wd_now_ns();
    uint64_t uploaded_pixels = 0;
    uint64_t uploaded_rects = 0;
    bool ok = true;

    {
        std::lock_guard<std::mutex> framebuffer_lock(state.framebuffer_mutex);
        const size_t expected_pixels = static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height);
        if (frame_width == 0 || frame_height == 0 || state.framebuffer.size() < expected_pixels)
        {
            return false;
        }

        for (const ClientDirtyRect& rect : rects)
        {
            if (rect.w == 0 || rect.h == 0 || rect.x >= frame_width || rect.y >= frame_height)
            {
                continue;
            }

            const uint32_t visible_width  = std::min<uint32_t>(rect.w, frame_width - rect.x);
            const uint32_t visible_height = std::min<uint32_t>(rect.h, frame_height - rect.y);
            if (visible_width == 0 || visible_height == 0)
            {
                continue;
            }

            const SDL_Rect sdl_rect{static_cast<int>(rect.x), static_cast<int>(rect.y), static_cast<int>(visible_width),
                                    static_cast<int>(visible_height)};
            const uint32_t* pixels = state.framebuffer.data() + static_cast<size_t>(rect.y) * frame_width + rect.x;
            if (!SDL_UpdateTexture(texture, &sdl_rect, pixels, static_cast<int>(frame_width * WD_BYTES_PER_PIXEL)))
            {
                ok = false;
                break;
            }

            uploaded_pixels += static_cast<uint64_t>(visible_width) * static_cast<uint64_t>(visible_height);
            ++uploaded_rects;
        }
    }

    if (ok)
    {
        state.stats.sdl_texture_partial_uploads.fetch_add(1, std::memory_order_relaxed);
        state.stats.sdl_texture_dirty_rects.fetch_add(uploaded_rects, std::memory_order_relaxed);
        record_texture_upload_stats(state, started_ns, uploaded_pixels);
    }

    return ok;
}

bool client_send_keyboard_key_checked(ClientState& state, uint16_t evdev_key_code, bool pressed) {
    if (client_send_keyboard_key(state, evdev_key_code, pressed))
    {
        return true;
    }

    WD_LOG_ERROR("failed to send keyboard event evdev=%u pressed=%u", evdev_key_code, pressed ? 1 : 0);
    state.running.store(false, std::memory_order_relaxed);
    return false;
}

void release_forwarded_keyboard_keys(ClientState& state) {
    for (size_t i = 0; i < g_forwarded_keys.size(); ++i)
    {
        if (!g_forwarded_keys[i])
        {
            continue;
        }

        const auto     scancode       = static_cast<SDL_Scancode>(i);
        const uint16_t evdev_key_code = sdl_scancode_to_evdev(scancode);

        if (evdev_key_code != 0)
        {
            client_send_keyboard_key_checked(state, evdev_key_code, false);
        }

        g_forwarded_keys[i] = false;
    }

    g_suppress_paste_v_keyup = false;
}


bool window_render_feedback_visible(SDL_Window* window) {
    if (!window)
    {
        return true;
    }

    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0;
}

void update_render_feedback_visibility(ClientState& state, SDL_Window* window) {
    state.render_feedback_visible.store(window_render_feedback_visible(window), std::memory_order_relaxed);
}

void handle_sdl_event(ClientState& state, const SDL_Event& event) {
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
        release_forwarded_keyboard_keys(state);
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat)
        {
            return;
        }

        const SDL_Scancode scancode       = event.key.scancode;
        const uint16_t     evdev_key_code = sdl_scancode_to_evdev(scancode);

        if (evdev_key_code == 0)
        {
            return;
        }

        const bool pressed = event.type == SDL_EVENT_KEY_DOWN;

        if (scancode == SDL_SCANCODE_V)
        {
            if (pressed && (SDL_GetModState() & SDL_KMOD_CTRL))
            {
                /*
                 * Ctrl+V is a host-clipboard paste command. Send the clipboard
                 * payload, but do not forward the V key itself: the server will
                 * publish the selection, then synthesize V while Ctrl is already
                 * held remotely. Forwarding V here races ahead of publication.
                 */
                send_host_clipboard_to_server(state, false);
                g_suppress_paste_v_keyup = true;
                return;
            }

            if (!pressed && g_suppress_paste_v_keyup)
            {
                g_suppress_paste_v_keyup = false;
                return;
            }
        }

        bool& forwarded = g_forwarded_keys[static_cast<size_t>(scancode)];

        if (pressed == forwarded)
        {
            return;
        }

        if (client_send_keyboard_key_checked(state, evdev_key_code, pressed))
        {
            forwarded = pressed;
        }

        return;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION)
    {
        wd_pointer_event_payload pointer{};
        pointer.session_id          = state.config.session_id;
        pointer.client_timestamp_ns = wd_now_ns();
        pointer.event_type          = WD_POINTER_EVENT_MOTION;
        pointer.x                   = map_mouse_coord_x(event.motion.x);
        pointer.y                   = map_mouse_coord_y(event.motion.y);
        pointer.modifiers           = current_pointer_modifiers();

        if (!client_send_pointer_event(state, pointer))
        {
            WD_LOG_ERROR("failed to send pointer motion");
            state.running.store(false, std::memory_order_relaxed);
        }

        return;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
    {
        const uint16_t linux_button = sdl_button_to_linux_button(event.button.button);

        if (linux_button == 0)
        {
            return;
        }

        wd_pointer_event_payload pointer{};
        pointer.session_id          = state.config.session_id;
        pointer.client_timestamp_ns = wd_now_ns();
        pointer.event_type          = WD_POINTER_EVENT_BUTTON;
        pointer.x                   = map_mouse_coord_x(event.button.x);
        pointer.y                   = map_mouse_coord_y(event.button.y);
        pointer.button              = linux_button;
        pointer.button_state        = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? WD_POINTER_BUTTON_PRESSED : WD_POINTER_BUTTON_RELEASED;
        pointer.modifiers           = current_pointer_modifiers();

        if (linux_button == WD_BTN_MIDDLE && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            /*
             * Publish the host clipboard as primary selection first, then forward
             * the middle click so the Wayland client performs its normal primary
             * paste request.
             */
            send_host_clipboard_to_server(state, true);
        }

        if (!client_send_pointer_event(state, pointer))
        {
            WD_LOG_ERROR("failed to send pointer button");
            state.running.store(false, std::memory_order_relaxed);
        }

        return;
    }

    if (event.type == SDL_EVENT_MOUSE_WHEEL)
    {
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);

        if (event.wheel.y != 0)
        {
            wd_pointer_event_payload pointer{};
            pointer.session_id          = state.config.session_id;
            pointer.client_timestamp_ns = wd_now_ns();
            pointer.event_type          = WD_POINTER_EVENT_AXIS;
            pointer.x                   = map_mouse_coord_x(mouse_x);
            pointer.y                   = map_mouse_coord_y(mouse_y);
            pointer.axis                = WD_POINTER_AXIS_VERTICAL;
            pointer.modifiers           = current_pointer_modifiers();

            /*
             * Wayland scroll convention: negative value usually means scroll down.
             * SDL wheel y > 0 means wheel up.
             */
            pointer.axis_value = static_cast<int32_t>(-event.wheel.y * WD_CLIENT_WHEEL_AXIS_STEP);

            if (!client_send_pointer_event(state, pointer))
            {
                WD_LOG_ERROR("failed to send vertical pointer axis");
                state.running.store(false, std::memory_order_relaxed);
            }
        }

        if (event.wheel.x != 0)
        {
            wd_pointer_event_payload pointer{};
            pointer.session_id          = state.config.session_id;
            pointer.client_timestamp_ns = wd_now_ns();
            pointer.event_type          = WD_POINTER_EVENT_AXIS;
            pointer.x                   = map_mouse_coord_x(mouse_x);
            pointer.y                   = map_mouse_coord_y(mouse_y);
            pointer.axis                = WD_POINTER_AXIS_HORIZONTAL;
            pointer.axis_value          = static_cast<int32_t>(event.wheel.x * WD_CLIENT_WHEEL_AXIS_STEP);
            pointer.modifiers           = current_pointer_modifiers();

            if (!client_send_pointer_event(state, pointer))
            {
                WD_LOG_ERROR("failed to send horizontal pointer axis");
                state.running.store(false, std::memory_order_relaxed);
            }
        }

        return;
    }
}

bool present_sdl_frame(ClientState& state, SDL_Renderer* renderer, SDL_Texture* texture, const ContextMenu& context_menu,
                       bool remote_texture_updated, std::vector<uint64_t>& present_tile_timestamps,
                       std::vector<uint64_t>& present_input_sequences, uint64_t& last_present_ns) {
    if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255))
    {
        return log_sdl_error("SDL_SetRenderDrawColor");
    }

    if (!SDL_RenderClear(renderer))
    {
        return log_sdl_error("SDL_RenderClear");
    }

    if (!SDL_RenderTexture(renderer, texture, nullptr, &g_content_rect))
    {
        return log_sdl_error("SDL_RenderTexture");
    }

    render_context_menu(renderer, context_menu);

    const uint64_t present_started_ns = wd_now_ns();
    if (!SDL_RenderPresent(renderer))
    {
        return log_sdl_error("SDL_RenderPresent");
    }

    const uint64_t present_elapsed_ns = wd_now_ns() - present_started_ns;
    state.stats.sdl_render_frames.fetch_add(1, std::memory_order_relaxed);
    if (remote_texture_updated)
    {
        state.stats.sdl_remote_frames.fetch_add(1, std::memory_order_relaxed);
    }
    state.stats.sdl_present_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_present_sum_ns.fetch_add(present_elapsed_ns, std::memory_order_relaxed);
    record_atomic_max(state.stats.sdl_present_max_ns, present_elapsed_ns);

    {
        std::lock_guard<std::mutex> present_lock(state.present_mutex);
        present_tile_timestamps.swap(state.pending_present_tile_timestamps);
        present_input_sequences.swap(state.pending_present_input_sequences);
    }

    const uint64_t present_ns = wd_now_ns();
    last_present_ns          = present_ns;
    for (uint64_t tile_timestamp_ns : present_tile_timestamps)
    {
        if (tile_timestamp_ns != 0 && present_ns >= tile_timestamp_ns)
        {
            state.stats.tile_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.tile_present_latency_sum_ns.fetch_add(present_ns - tile_timestamp_ns, std::memory_order_relaxed);
        }
    }
    for (uint64_t sequence : present_input_sequences)
    {
        uint64_t input_timestamp_ns = 0;
        if (take_input_timestamp(state, sequence, input_timestamp_ns) && present_ns >= input_timestamp_ns)
        {
            state.stats.input_sequence_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.input_sequence_present_latency_sum_ns.fetch_add(present_ns - input_timestamp_ns, std::memory_order_relaxed);
        }
    }
    present_tile_timestamps.clear();
    present_input_sequences.clear();

    const uint64_t input_timestamp_ns = state.stats.latest_input_event_timestamp_ns.exchange(0, std::memory_order_relaxed);
    if (input_timestamp_ns != 0 && present_ns >= input_timestamp_ns)
    {
        state.stats.input_to_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
        state.stats.input_to_present_latency_sum_ns.fetch_add(present_ns - input_timestamp_ns, std::memory_order_relaxed);
    }

    return true;
}

} // namespace

int run_sdl_viewer(ClientState& state) {
    g_client_config = &state.config;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        WD_LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("WayDisplay Client", state.config.width, state.config.height,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);

    if (!window)
    {
        WD_LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!SDL_SetWindowMinimumSize(window, WD_CLIENT_MIN_WINDOW_WIDTH, WD_CLIENT_MIN_WINDOW_HEIGHT))
    {
        log_sdl_warning("SDL_SetWindowMinimumSize");
    }
    update_window_size(window);
    update_render_feedback_visibility(state, window);

    const char* renderer_driver = "vulkan";
    WD_LOG_INFO("SDL renderer vsync: %s", state.stream_config.disable_vsync ? "disabled" : "enabled");
    WD_LOG_INFO("SDL local present cap: %u fps", (unsigned)client_local_present_fps(state));

    SDL_Renderer* renderer = SDL_CreateRenderer(window, renderer_driver);

    if (!renderer)
    {
        WD_LOG_ERROR("SDL_CreateRenderer(%s) failed: %s", renderer_driver, SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const char* renderer_name = SDL_GetRendererName(renderer);
    WD_LOG_INFO("SDL renderer: requested=%s active=%s", renderer_driver, renderer_name ? renderer_name : "unknown");

    if (!SDL_SetRenderVSync(renderer, state.stream_config.disable_vsync ? SDL_RENDERER_VSYNC_DISABLED : 1))
    {
        WD_LOG_ERROR("SDL_SetRenderVSync failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND))
    {
        WD_LOG_ERROR("SDL_SetRenderDrawBlendMode failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture* texture = create_frame_texture(renderer, state.config.width, state.config.height);

    if (!texture)
    {
        WD_LOG_ERROR("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::thread udp_thread(udp_reader_main, &state);

    uint64_t              last_stats_ns           = wd_now_ns();
    uint64_t              last_stats_log_ns       = last_stats_ns;
    bool                  frame_dirty             = true;
    bool                  texture_needs_full_upload = true;
    uint16_t              last_requested_width    = state.config.width;
    uint16_t              last_requested_height   = state.config.height;
    uint16_t              pending_resize_width    = 0;
    uint16_t              pending_resize_height   = 0;
    uint64_t              pending_resize_since_ns = 0;
    uint64_t              last_present_ns         = 0;
    ContextMenu           context_menu;
    std::vector<uint64_t> present_tile_timestamps;
    std::vector<uint64_t> present_input_sequences;
    std::vector<ClientDirtyRect> dirty_rects;

    while (state.running.load(std::memory_order_relaxed))
    {
        apply_pending_cursor_shape(state);
        drain_remote_selection_updates(state);

        if (apply_pending_server_config(state, window, renderer, texture))
        {
            last_requested_width    = state.config.width;
            last_requested_height   = state.config.height;
            pending_resize_width    = 0;
            pending_resize_height   = 0;
            pending_resize_since_ns = 0;
            present_tile_timestamps.clear();
            frame_dirty = true;
            texture_needs_full_upload = true;
        }

        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                release_forwarded_keyboard_keys(state);
                state.running.store(false, std::memory_order_relaxed);
                break;
            }

            if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
            {
                update_render_feedback_visibility(state, window);
            }

            if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                update_window_size(window);
                frame_dirty = true;

                if (g_window_width > 0 && g_window_height > 0 && g_window_width <= 65535 && g_window_height <= 65535)
                {
                    const auto width  = static_cast<uint16_t>(g_window_width);
                    const auto height = static_cast<uint16_t>(g_window_height);

                    if (width != last_requested_width || height != last_requested_height)
                    {
                        pending_resize_width    = width;
                        pending_resize_height   = height;
                        pending_resize_since_ns = wd_now_ns();
                    }
                }

                continue;
            }

            if (handle_context_menu_event(state, window, context_menu, event, frame_dirty))
            {
                continue;
            }

            handle_sdl_event(state, event);
        }

        if (pending_resize_width != 0 && pending_resize_height != 0)
        {
            const uint64_t now = wd_now_ns();

            if (now - pending_resize_since_ns >= WD_CLIENT_RESIZE_DEBOUNCE_NS)
            {
                last_requested_width    = pending_resize_width;
                last_requested_height   = pending_resize_height;
                pending_resize_width    = 0;
                pending_resize_height   = 0;
                pending_resize_since_ns = 0;

                if (!client_send_display_resize(state, last_requested_width, last_requested_height))
                {
                    WD_LOG_ERROR("failed to send display resize request");
                }
            }
        }

        const bool local_frame_dirty = frame_dirty || texture_needs_full_upload;
        bool       remote_frame_dirty = false;
        bool       remote_texture_updated = false;

        const bool remote_dirty_pending = state.pending_dirty_rect_count.load(std::memory_order_acquire) != 0;
        if (local_frame_dirty || remote_dirty_pending)
        {
            const uint64_t present_check_ns = wd_now_ns();
            remote_frame_dirty = remote_dirty_pending &&
                                 (local_frame_dirty || client_local_present_due(state, last_present_ns, present_check_ns));
        }

        if (local_frame_dirty || remote_frame_dirty)
        {
            const uint32_t frame_width  = state.config.width;
            const uint32_t frame_height = state.config.height;

            if (texture_needs_full_upload || remote_frame_dirty)
            {
                dirty_rects.clear();
                if (!texture_needs_full_upload)
                {
                    std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
                    dirty_rects.swap(state.pending_dirty_rects);
                    state.pending_dirty_rect_count.store(0, std::memory_order_release);
                }

                bool used_bounds_upload = false;
                const uint64_t source_dirty_rect_count = dirty_rects.size();
                if (remote_frame_dirty && !texture_needs_full_upload && source_dirty_rect_count == 0)
                {
                    state.stats.sdl_empty_remote_wakeups.fetch_add(1, std::memory_order_relaxed);
                    remote_frame_dirty = false;
                }
                if (!texture_needs_full_upload && source_dirty_rect_count != 0)
                {
                    coalesce_dirty_texture_rects(dirty_rects, frame_width, frame_height, used_bounds_upload);
                }

                const bool upload_full = texture_needs_full_upload || should_upload_full_texture(state, dirty_rects);
                if (source_dirty_rect_count != 0)
                {
                    state.stats.sdl_texture_source_dirty_rects.fetch_add(source_dirty_rect_count, std::memory_order_relaxed);
                    state.stats.sdl_texture_coalesced_dirty_rects.fetch_add(dirty_rects.size(), std::memory_order_relaxed);
                    if (used_bounds_upload)
                    {
                        state.stats.sdl_texture_bounds_uploads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (upload_full)
                {
                    std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
                    state.pending_dirty_rects.clear();
                    state.pending_dirty_rect_count.store(0, std::memory_order_release);
                }
                const bool texture_updated = upload_full ? upload_full_texture(state, texture, frame_width, frame_height)
                                                         : upload_dirty_texture_rects(state, texture, dirty_rects, frame_width, frame_height);

                if (!texture_updated)
                {
                    WD_LOG_ERROR("failed to update SDL texture: %s", SDL_GetError());
                    state.running.store(false, std::memory_order_relaxed);
                    break;
                }
                remote_texture_updated = remote_frame_dirty && source_dirty_rect_count != 0;
            }

            const bool should_present = local_frame_dirty || remote_texture_updated;
            if (should_present)
            {
                if (!present_sdl_frame(state, renderer, texture, context_menu, remote_texture_updated, present_tile_timestamps,
                                       present_input_sequences, last_present_ns))
                {
                    state.running.store(false, std::memory_order_relaxed);
                    break;
                }

                frame_dirty               = false;
                texture_needs_full_upload = false;
            }
        }

        const uint64_t now = wd_now_ns();
        if (now - last_stats_ns >= WD_CLIENT_STATS_FEEDBACK_INTERVAL_NS)
        {
            const bool log_stats = now - last_stats_log_ns >= WD_STATS_LOG_INTERVAL_NS;
            sample_client_stats(state, log_stats);
            last_stats_ns = now;
            if (log_stats)
            {
                last_stats_log_ns = now;
            }
        }

        const bool pending_remote_dirty = state.pending_dirty_rect_count.load(std::memory_order_acquire) != 0;
        if (!frame_dirty && !pending_remote_dirty)
        {
            SDL_Delay(WD_CLIENT_FRAME_DELAY_MS);
        }
        else if (!frame_dirty)
        {
            const uint32_t delay_ms = client_present_delay_ms(state, last_present_ns, wd_now_ns());
            if (delay_ms != 0)
            {
                SDL_Delay(delay_ms);
            }
        }
    }

    state.running.store(false, std::memory_order_relaxed);
    if (udp_thread.joinable())
    {
        udp_thread.join();
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    free_cached_cursors();
    SDL_Quit();

    return 0;
}

} // namespace waydisplay
