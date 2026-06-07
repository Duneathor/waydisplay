#include "sdl_viewer.hpp"

#include "client_net.hpp"
#include "sdl_input.hpp"
#include "tile_reassembly.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

#include <SDL2/SDL.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace waydisplay {
namespace {

const wd_server_config_payload*     g_client_config = nullptr;
int                                 g_window_width  = 1;
int                                 g_window_height = 1;
SDL_Rect                            g_content_rect{0, 0, 1, 1};
std::array<bool, SDL_NUM_SCANCODES> g_forwarded_keys{};
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

void open_context_menu(ContextMenu& menu, int x, int y) {
    menu.open  = true;
    menu.x     = x;
    menu.y     = y;
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

int context_menu_hit_test(const ContextMenu& menu, int x, int y) {
    if (!menu.open)
    {
        return -1;
    }

    if (x < menu.x || x >= menu.x + WD_CLIENT_CONTEXT_MENU_WIDTH)
    {
        return -1;
    }

    const int content_y = y - menu.y - WD_CLIENT_CONTEXT_MENU_PADDING_Y;
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

                SDL_Rect px{cursor_x + col * scale, y + row * scale, scale, scale};
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

    SDL_Rect shadow{menu.x + 5, menu.y + 5, WD_CLIENT_CONTEXT_MENU_WIDTH, height};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_RenderFillRect(renderer, &shadow);

    SDL_Rect bg{menu.x, menu.y, WD_CLIENT_CONTEXT_MENU_WIDTH, height};
    SDL_SetRenderDrawColor(renderer, 26, 29, 33, 248);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 72, 78, 86, 255);
    SDL_RenderDrawRect(renderer, &bg);

    SDL_Rect inner{menu.x + 1, menu.y + 1, WD_CLIENT_CONTEXT_MENU_WIDTH - 2, height - 2};
    SDL_SetRenderDrawColor(renderer, 43, 47, 53, 255);
    SDL_RenderDrawRect(renderer, &inner);

    for (int i = 0; i < static_cast<int>(CONTEXT_MENU_ITEMS.size()); ++i)
    {
        const SDL_Rect item_rect{
            menu.x + WD_CLIENT_CONTEXT_MENU_PADDING_X,
            menu.y + WD_CLIENT_CONTEXT_MENU_PADDING_Y + i * WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT,
            WD_CLIENT_CONTEXT_MENU_WIDTH - WD_CLIENT_CONTEXT_MENU_PADDING_X * 2,
            WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT,
        };

        if (i == menu.hover && CONTEXT_MENU_ITEMS[i].enabled)
        {
            SDL_SetRenderDrawColor(renderer, 56, 116, 186, 255);
            SDL_RenderFillRect(renderer, &item_rect);
        }

        if (i == 2)
        {
            SDL_SetRenderDrawColor(renderer, 66, 70, 76, 255);
            SDL_RenderDrawLine(renderer, menu.x + WD_CLIENT_CONTEXT_MENU_PADDING_X, item_rect.y - 1,
                               menu.x + WD_CLIENT_CONTEXT_MENU_WIDTH - WD_CLIENT_CONTEXT_MENU_PADDING_X, item_rect.y - 1);
        }

        draw_text(renderer, CONTEXT_MENU_ITEMS[i].label, item_rect.x + WD_CLIENT_CONTEXT_MENU_TEXT_X,
                  item_rect.y + WD_CLIENT_CONTEXT_MENU_TEXT_Y, WD_CLIENT_CONTEXT_MENU_TEXT_SCALE, CONTEXT_MENU_ITEMS[i].enabled);
    }
}

void execute_context_menu_action(ClientState& state, SDL_Window* window, ContextMenuAction action) {
    switch (action)
    {
    case ContextMenuAction::ToggleFullscreen: {
        const uint32_t flags      = SDL_GetWindowFlags(window);
        const bool     fullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        update_window_size(window);
        break;
    }

    case ContextMenuAction::ActualSize:
        SDL_SetWindowFullscreen(window, 0);
        SDL_SetWindowSize(window, state.config.width, state.config.height);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
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
    if (event.type != SDL_MOUSEBUTTONDOWN || event.button.button != SDL_BUTTON_RIGHT)
    {
        return false;
    }

    const SDL_Keymod mods = SDL_GetModState();
    return (mods & KMOD_CTRL) && (mods & KMOD_ALT);
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

    if (event.type == SDL_MOUSEMOTION)
    {
        const int hover = context_menu_hit_test(menu, event.motion.x, event.motion.y);
        if (hover != menu.hover)
        {
            menu.hover      = hover;
            out_frame_dirty = true;
        }
        return true;
    }

    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
    {
        close_context_menu(menu);
        out_frame_dirty = true;
        return true;
    }

    if (event.type == SDL_MOUSEBUTTONDOWN)
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

    if (event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL)
    {
        return true;
    }

    return false;
}

uint16_t current_pointer_modifiers() {
    const SDL_Keymod mods = SDL_GetModState();

    uint16_t result = 0;

    if (mods & KMOD_ALT)
    {
        result |= WD_POINTER_MOD_ALT;
    }

    if (mods & KMOD_SHIFT)
    {
        result |= WD_POINTER_MOD_SHIFT;
    }

    if (mods & KMOD_CTRL)
    {
        result |= WD_POINTER_MOD_CTRL;
    }

    if (mods & KMOD_GUI)
    {
        result |= WD_POINTER_MOD_SUPER;
    }

    return result;
}

bool send_host_clipboard_to_server(ClientState& state, bool primary) {
    char* text = SDL_GetClipboardText();
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
        SDL_SetClipboardText(clipboard.c_str());
    }

    if (have_primary)
    {
        /* SDL2 does not expose a primary-selection API. Keep this for future
         * backends or an SDL3 migration, but don't discard the remote update
         * silently in the networking layer. */
        WD_LOG_DEBUG("remote primary selection updated: %zu bytes", primary.size());
    }
}

SDL_SystemCursor sdl_cursor_for_wd_shape(uint16_t shape) {
    switch (shape)
    {
    case WD_CURSOR_SHAPE_POINTER:
        return SDL_SYSTEM_CURSOR_HAND;

    case WD_CURSOR_SHAPE_TEXT:
        return SDL_SYSTEM_CURSOR_IBEAM;

    case WD_CURSOR_SHAPE_MOVE:
        return SDL_SYSTEM_CURSOR_SIZEALL;

    case WD_CURSOR_SHAPE_EW_RESIZE:
        return SDL_SYSTEM_CURSOR_SIZEWE;

    case WD_CURSOR_SHAPE_NS_RESIZE:
        return SDL_SYSTEM_CURSOR_SIZENS;

    case WD_CURSOR_SHAPE_NWSE_RESIZE:
        return SDL_SYSTEM_CURSOR_SIZENWSE;

    case WD_CURSOR_SHAPE_NESW_RESIZE:
        return SDL_SYSTEM_CURSOR_SIZENESW;

    case WD_CURSOR_SHAPE_WAIT:
        return SDL_SYSTEM_CURSOR_WAIT;

    case WD_CURSOR_SHAPE_NOT_ALLOWED:
        return SDL_SYSTEM_CURSOR_NO;

    case WD_CURSOR_SHAPE_DEFAULT:
    default:
        return SDL_SYSTEM_CURSOR_ARROW;
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

    if (!cursors[shape])
    {
        cursors[shape] = SDL_CreateSystemCursor(sdl_cursor_for_wd_shape(shape));
    }

    if (cursors[shape])
    {
        SDL_SetCursor(cursors[shape]);
        SDL_ShowCursor(SDL_ENABLE);
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

double avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return static_cast<double>(sum_ns) / static_cast<double>(samples) / 1000000.0;
}

void print_client_stats(ClientState& state) {
    const uint64_t udp_packets              = take_stat(state.stats.udp_packets_rx);
    const uint64_t udp_bytes                = take_stat(state.stats.udp_bytes_rx);
    const uint64_t invalid                  = take_stat(state.stats.udp_ignored_invalid);
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
    const uint64_t summary_to_retx_samples  = take_stat(state.stats.summary_to_retx_samples);
    const uint64_t summary_to_retx_sum_ns   = take_stat(state.stats.summary_to_retx_sum_ns);
    const uint64_t keys                     = take_stat(state.stats.tcp_keyboard_tx);
    const uint64_t pointer                  = take_stat(state.stats.tcp_pointer_tx);
    const uint64_t input_events             = take_stat(state.stats.tcp_input_events_tx);
    const uint64_t input_channel_events       = take_stat(state.stats.tcp_input_channel_tx);
    const uint64_t input_fallback_events      = take_stat(state.stats.tcp_input_channel_fallback_tx);
    const uint64_t selection_channel_events   = take_stat(state.stats.tcp_selection_channel_tx);
    const uint64_t selection_fallback_events  = take_stat(state.stats.tcp_selection_channel_fallback_tx);
    const uint64_t summary_latency_samples  = take_stat(state.stats.summary_latency_samples);
    const uint64_t summary_latency_sum_ns   = take_stat(state.stats.summary_latency_sum_ns);
    const uint64_t tile_assembly_samples    = take_stat(state.stats.tile_assembly_samples);
    const uint64_t tile_assembly_sum_ns     = take_stat(state.stats.tile_assembly_sum_ns);
    const uint64_t tile_present_samples     = take_stat(state.stats.tile_present_latency_samples);
    const uint64_t tile_present_sum_ns      = take_stat(state.stats.tile_present_latency_sum_ns);
    const uint64_t input_to_present_samples = take_stat(state.stats.input_to_present_latency_samples);
    const uint64_t input_to_present_sum_ns  = take_stat(state.stats.input_to_present_latency_sum_ns);

    /*
     * Periodic generation summaries are intentionally sent even while idle so
     * lossy links can eventually repair state. Do not log a stats line when
     * summaries are the only activity.
     */
    const bool useful_activity = udp_packets != 0 || udp_bytes != 0 || completed != 0 || partial_timeouts != 0 ||
                                 partial_missing_packets != 0 || partial_retx_queued != 0 || retx_response_samples != 0 ||
                                 timeout_updates != 0 || invalid != 0 || old_gen != 0 ||
                                 retx != 0 || summary_retx_queued != 0 || keys != 0 || pointer != 0 || input_events != 0 ||
                                 input_channel_events != 0 || input_fallback_events != 0 || selection_channel_events != 0 ||
                                 selection_fallback_events != 0;

    if (!useful_activity)
    {
        return;
    }

    wd_client_stats_payload feedback{};
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        feedback.session_id = state.config.session_id;
    }
    feedback.udp_packets_rx             = udp_packets;
    feedback.udp_bytes_rx               = udp_bytes;
    feedback.udp_tiles_completed        = completed;
    feedback.udp_completed_packets      = completed_packets;
    feedback.partial_tiles_timed_out    = partial_timeouts;
    feedback.udp_ignored_old_generation = old_gen;
    feedback.retx_requests_tx           = retx;
    if (feedback.session_id != 0)
    {
        client_send_stats(state, feedback);
    }

    WD_LOG_DEBUG(
        "[client stats/s] udp_pkts=%llu udp_kib=%.1f completed_tiles=%llu "
        "udp_kib_per_completed_tile=%.2f compressed_kib_per_completed_tile=%.2f pkts_per_completed_tile=%.2f "
        "partial_timeouts=%llu partial_missing_pkts=%llu partial_retx_queued=%llu "
        "invalid=%llu old_gen=%llu summaries=%llu retx_req=%llu summary_retx_tiles_queued=%llu keys=%llu "
        "pointer=%llu input_events=%llu input_channel_events=%llu input_fallback_events=%llu "
        "selection_channel_events=%llu selection_fallback_events=%llu "
        "summary_rx_avg_ms=%.2f summary_to_retx_avg_ms=%.2f retx_response_avg_ms=%.2f "
        "tile_assembly_avg_ms=%.2f tile_reassembly_timeout_ms=%.2f timeout_updates=%llu "
        "tile_present_avg_ms=%.2f input_to_present_avg_ms=%.2f",
        static_cast<unsigned long long>(udp_packets), static_cast<double>(udp_bytes) / 1024.0, static_cast<unsigned long long>(completed),
        completed ? (static_cast<double>(udp_bytes) / 1024.0) / static_cast<double>(completed) : 0.0,
        completed ? (static_cast<double>(completed_compressed) / 1024.0) / static_cast<double>(completed) : 0.0,
        completed ? static_cast<double>(completed_packets) / static_cast<double>(completed) : 0.0,
        static_cast<unsigned long long>(partial_timeouts), static_cast<unsigned long long>(partial_missing_packets),
        static_cast<unsigned long long>(partial_retx_queued), static_cast<unsigned long long>(invalid),
        static_cast<unsigned long long>(old_gen), static_cast<unsigned long long>(summaries), static_cast<unsigned long long>(retx),
        static_cast<unsigned long long>(summary_retx_queued), static_cast<unsigned long long>(keys), static_cast<unsigned long long>(pointer),
        static_cast<unsigned long long>(input_events), static_cast<unsigned long long>(input_channel_events),
        static_cast<unsigned long long>(input_fallback_events), static_cast<unsigned long long>(selection_channel_events),
        static_cast<unsigned long long>(selection_fallback_events), avg_ms(summary_latency_sum_ns, summary_latency_samples),
        avg_ms(summary_to_retx_sum_ns, summary_to_retx_samples), avg_ms(retx_response_sum_ns, retx_response_samples),
        avg_ms(tile_assembly_sum_ns, tile_assembly_samples),
        static_cast<double>(state.tile_reassembly_timeout_ns.load(std::memory_order_relaxed)) / 1000000.0,
        static_cast<unsigned long long>(timeout_updates), avg_ms(tile_present_sum_ns, tile_present_samples),
        avg_ms(input_to_present_sum_ns, input_to_present_samples));
}

bool blit_tile_xrgb8888(ClientState& state, uint16_t tile_id, const std::vector<uint8_t>& tile_bytes) {
    if (tile_id >= state.config.total_tiles)
    {
        return false;
    }

    const uint32_t tile_x = tile_id % state.config.tiles_x;
    const uint32_t tile_y = tile_id / state.config.tiles_x;
    const uint32_t dst_x  = tile_x * state.config.tile_width;
    const uint32_t dst_y  = tile_y * state.config.tile_height;
    const size_t   expected_size =
        static_cast<size_t>(state.config.tile_width) * static_cast<size_t>(state.config.tile_height) * WD_BYTES_PER_PIXEL;

    if (tile_bytes.size() < expected_size || dst_x >= state.config.width || dst_y >= state.config.height)
    {
        return false;
    }

    const uint32_t visible_width  = std::min<uint32_t>(state.config.tile_width, state.config.width - dst_x);
    const uint32_t visible_height = std::min<uint32_t>(state.config.tile_height, state.config.height - dst_y);

    for (uint32_t y = 0; y < visible_height; ++y)
    {
        const uint8_t* src = tile_bytes.data() + static_cast<size_t>(y) * state.config.tile_width * WD_BYTES_PER_PIXEL;
        uint32_t*      dst = state.framebuffer.data() + static_cast<size_t>(dst_y + y) * state.config.width + dst_x;

        std::memcpy(dst, src, static_cast<size_t>(visible_width) * WD_BYTES_PER_PIXEL);
    }

    return true;
}

bool drain_udp(ClientState& state, TileReassembler& reassembler, bool& out_frame_dirty,
               std::vector<uint64_t>& out_present_tile_timestamps) {
    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    const size_t recvbuf_size = sizeof(wd_udp_tile_packet_header) + udp_payload_target + 512;

    if (state.udp_recv_buffer.size() < recvbuf_size)
    {
        state.udp_recv_buffer.assign(recvbuf_size, 0);
    }

    for (;;)
    {
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

            std::perror("recv UDP");
            return false;
        }

        if (n == 0)
        {
            return true;
        }

        state.stats.udp_packets_rx.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_bytes_rx.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

        CompletedTile completed = reassembler.process_udp_packet(state, state.udp_recv_buffer.data(), static_cast<size_t>(n));

        if (!completed.valid)
        {
            continue;
        }

        if (!blit_tile_xrgb8888(state, completed.tile_id, completed.tile_bytes))
        {
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state.generation_mutex);

            if (completed.generation > state.displayed_generation[completed.tile_id])
            {
                state.displayed_generation[completed.tile_id] = completed.generation;
            }
        }

        if (completed.first_packet_ns != 0 && completed.completed_timestamp_ns >= completed.first_packet_ns)
        {
            state.stats.tile_assembly_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.tile_assembly_sum_ns.fetch_add(completed.completed_timestamp_ns - completed.first_packet_ns,
                                                       std::memory_order_relaxed);
        }

        if (completed.completed_timestamp_ns != 0)
        {
            out_present_tile_timestamps.push_back(completed.completed_timestamp_ns);
        }

        state.stats.udp_completed_compressed_bytes.fetch_add(completed.compressed_size, std::memory_order_relaxed);
        state.stats.udp_completed_packets.fetch_add(completed.packet_count, std::memory_order_relaxed);
        state.stats.udp_tiles_completed.fetch_add(1, std::memory_order_relaxed);
        out_frame_dirty = true;
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

uint16_t map_mouse_coord_x(int x) {
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

    const int      local_x = x - g_content_rect.x;
    const uint32_t mapped =
        static_cast<uint32_t>((static_cast<uint64_t>(local_x) * g_client_config->width) / static_cast<uint32_t>(g_content_rect.w));

    if (mapped >= g_client_config->width)
    {
        return static_cast<uint16_t>(g_client_config->width - 1);
    }

    return static_cast<uint16_t>(mapped);
}

uint16_t map_mouse_coord_y(int y) {
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

    const int      local_y = y - g_content_rect.y;
    const uint32_t mapped =
        static_cast<uint32_t>((static_cast<uint64_t>(local_y) * g_client_config->height) / static_cast<uint32_t>(g_content_rect.h));

    if (mapped >= g_client_config->height)
    {
        return static_cast<uint16_t>(g_client_config->height - 1);
    }

    return static_cast<uint16_t>(mapped);
}

void update_window_size(SDL_Window* window) {
    int width  = 1;
    int height = 1;

    SDL_GetWindowSize(window, &width, &height);

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
        g_content_rect = SDL_Rect{0, 0, width, height};
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

    g_content_rect = SDL_Rect{
        (width - content_width) / 2,
        (height - content_height) / 2,
        content_width,
        content_height,
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

bool apply_pending_server_config(ClientState& state, SDL_Window* window, SDL_Renderer* renderer, SDL_Texture*& texture,
                                 TileReassembler& reassembler) {
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

    if (config.width == state.config.width && config.height == state.config.height && config.tiles_x == state.config.tiles_x &&
        config.tiles_y == state.config.tiles_y && config.total_tiles == state.config.total_tiles)
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        state.config = config;
        return false;
    }

    WD_LOG_INFO("server display resized: %ux%u tiles=%ux%u total=%u", config.width, config.height, config.tiles_x, config.tiles_y,
                config.total_tiles);

    std::vector<uint32_t> new_framebuffer(static_cast<size_t>(config.width) * config.height, 0);
    std::vector<uint64_t> new_displayed_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_queued_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_last_requested_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_last_request_ns(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_inflight_generation(config.total_tiles, 0);
    std::vector<uint64_t> new_retx_inflight_since_ns(config.total_tiles, 0);
    std::vector<uint8_t>  new_udp_recv_buffer(sizeof(wd_udp_tile_packet_header) + config.udp_payload_target + 512, 0);

    SDL_Texture* new_texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, config.width, config.height);

    if (!new_texture)
    {
        std::fprintf(stderr, "SDL_CreateTexture after resize failed: %s\n", SDL_GetError());
        state.running.store(false, std::memory_order_relaxed);
        return false;
    }

    {
        std::lock_guard<std::mutex> config_lock(state.config_mutex);
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

        state.config               = config;
        state.framebuffer          = std::move(new_framebuffer);
        state.displayed_generation = std::move(new_displayed_generation);
        state.retx_queue.clear();
        state.retx_queued_generation          = std::move(new_retx_queued_generation);
        state.retx_last_requested_generation = std::move(new_retx_last_requested_generation);
        state.retx_last_request_ns           = std::move(new_retx_last_request_ns);
        state.retx_inflight_generation       = std::move(new_retx_inflight_generation);
        state.retx_inflight_since_ns         = std::move(new_retx_inflight_since_ns);
        state.retx_request_tokens            = 0.0;
        state.retx_request_last_refill_ns    = 0;
        state.udp_recv_buffer                = std::move(new_udp_recv_buffer);
    }

    reassembler.reset();

    SDL_DestroyTexture(texture);
    texture = new_texture;

    g_client_config = &state.config;
    SDL_SetWindowMinimumSize(window, WD_CLIENT_MIN_WINDOW_WIDTH, WD_CLIENT_MIN_WINDOW_HEIGHT);
    update_window_size(window);

    return true;
}

bool client_send_keyboard_key_checked(ClientState& state, uint16_t evdev_key_code, bool pressed) {
    if (client_send_keyboard_key(state, evdev_key_code, pressed))
    {
        return true;
    }

    std::fprintf(stderr, "failed to send keyboard event evdev=%u pressed=%u\n", evdev_key_code, pressed ? 1 : 0);
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

void handle_sdl_event(ClientState& state, const SDL_Event& event) {
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
    {
        release_forwarded_keyboard_keys(state);
        return;
    }

    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
    {
        if (event.type == SDL_KEYDOWN && event.key.repeat != 0)
        {
            return;
        }

        const SDL_Scancode scancode       = event.key.keysym.scancode;
        const uint16_t     evdev_key_code = sdl_scancode_to_evdev(scancode);

        if (evdev_key_code == 0)
        {
            return;
        }

        const bool pressed = event.type == SDL_KEYDOWN;

        if (scancode == SDL_SCANCODE_V)
        {
            if (pressed && (SDL_GetModState() & KMOD_CTRL))
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

    if (event.type == SDL_MOUSEMOTION)
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
            std::fprintf(stderr, "failed to send pointer motion\n");
            state.running.store(false, std::memory_order_relaxed);
        }

        return;
    }

    if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
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
        pointer.button_state        = event.type == SDL_MOUSEBUTTONDOWN ? WD_POINTER_BUTTON_PRESSED : WD_POINTER_BUTTON_RELEASED;
        pointer.modifiers           = current_pointer_modifiers();

        if (linux_button == WD_BTN_MIDDLE && event.type == SDL_MOUSEBUTTONDOWN)
        {
            /*
             * Publish the host clipboard as primary selection first, then forward
             * the middle click so the Wayland client performs its normal primary
             * paste request.
             */
            send_host_clipboard_to_server(state, true);
        }

        if (linux_button == WD_BTN_RIGHT)
        {
            std::fprintf(stderr,
                         "WayDisplay client: sending right click %s "
                         "window=%d,%d remote=%u,%u mods=0x%x\n",
                         event.type == SDL_MOUSEBUTTONDOWN ? "press" : "release", event.button.x, event.button.y, pointer.x, pointer.y,
                         pointer.modifiers);
        }

        if (!client_send_pointer_event(state, pointer))
        {
            std::fprintf(stderr, "failed to send pointer button\n");
            state.running.store(false, std::memory_order_relaxed);
        }

        return;
    }

    if (event.type == SDL_MOUSEWHEEL)
    {
        int mouse_x = 0;
        int mouse_y = 0;
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
            pointer.axis_value = -event.wheel.y * WD_CLIENT_WHEEL_AXIS_STEP;

            if (!client_send_pointer_event(state, pointer))
            {
                std::fprintf(stderr, "failed to send vertical pointer axis\n");
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
            pointer.axis_value          = event.wheel.x * WD_CLIENT_WHEEL_AXIS_STEP;
            pointer.modifiers           = current_pointer_modifiers();

            if (!client_send_pointer_event(state, pointer))
            {
                std::fprintf(stderr, "failed to send horizontal pointer axis\n");
                state.running.store(false, std::memory_order_relaxed);
            }
        }

        return;
    }
}

} // namespace

int run_sdl_viewer(ClientState& state) {
    g_client_config = &state.config;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("WayDisplay Client", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, state.config.width,
                                          state.config.height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_SetWindowMinimumSize(window, WD_CLIENT_MIN_WINDOW_WIDTH, WD_CLIENT_MIN_WINDOW_HEIGHT);
    update_window_size(window);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer)
    {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_RenderSetLogicalSize(renderer, 0, 0);

    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, state.config.width, state.config.height);

    if (!texture)
    {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    TileReassembler reassembler;

    uint64_t              last_stats_ns           = wd_now_ns();
    bool                  frame_dirty             = true;
    uint16_t              last_requested_width    = state.config.width;
    uint16_t              last_requested_height   = state.config.height;
    uint16_t              pending_resize_width    = 0;
    uint16_t              pending_resize_height   = 0;
    uint64_t              pending_resize_since_ns = 0;
    ContextMenu           context_menu;
    std::vector<uint64_t> present_tile_timestamps;

    while (state.running.load(std::memory_order_relaxed))
    {
        apply_pending_cursor_shape(state);
        drain_remote_selection_updates(state);

        if (apply_pending_server_config(state, window, renderer, texture, reassembler))
        {
            last_requested_width    = state.config.width;
            last_requested_height   = state.config.height;
            pending_resize_width    = 0;
            pending_resize_height   = 0;
            pending_resize_since_ns = 0;
            present_tile_timestamps.clear();
            frame_dirty = true;
        }

        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
            {
                release_forwarded_keyboard_keys(state);
                state.running.store(false, std::memory_order_relaxed);
                break;
            }

            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESIZED))
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
                    std::fprintf(stderr, "failed to send display resize request\n");
                }
            }
        }

        if (!drain_udp(state, reassembler, frame_dirty, present_tile_timestamps))
        {
            state.running.store(false, std::memory_order_relaxed);
            break;
        }

        reassembler.expire_stale_entries(state);

        if (!client_flush_retransmit_requests(state))
        {
            std::fprintf(stderr, "failed to send retransmit request\n");
            state.running.store(false, std::memory_order_relaxed);
            break;
        }

        if (frame_dirty)
        {
            const uint32_t frame_width     = state.config.width;
            const uint32_t frame_height    = state.config.height;
            const size_t   expected_pixels = static_cast<size_t>(frame_width) * frame_height;

            if (frame_width == 0 || frame_height == 0 || state.framebuffer.size() < expected_pixels ||
                SDL_UpdateTexture(texture, nullptr, state.framebuffer.data(), static_cast<int>(frame_width * WD_BYTES_PER_PIXEL)) != 0)
            {
                std::fprintf(stderr, "failed to update SDL texture: %s\n", SDL_GetError());
                state.running.store(false, std::memory_order_relaxed);
                break;
            }

            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, &g_content_rect);
            render_context_menu(renderer, context_menu);
            SDL_RenderPresent(renderer);

            const uint64_t present_ns = wd_now_ns();
            for (uint64_t tile_timestamp_ns : present_tile_timestamps)
            {
                if (tile_timestamp_ns != 0 && present_ns >= tile_timestamp_ns)
                {
                    state.stats.tile_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
                    state.stats.tile_present_latency_sum_ns.fetch_add(present_ns - tile_timestamp_ns, std::memory_order_relaxed);
                }
            }
            present_tile_timestamps.clear();

            const uint64_t input_timestamp_ns = state.stats.latest_input_event_timestamp_ns.exchange(0, std::memory_order_relaxed);
            if (input_timestamp_ns != 0 && present_ns >= input_timestamp_ns)
            {
                state.stats.input_to_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
                state.stats.input_to_present_latency_sum_ns.fetch_add(present_ns - input_timestamp_ns, std::memory_order_relaxed);
            }

            frame_dirty = false;
        }

        const uint64_t now = wd_now_ns();
        if (now - last_stats_ns >= WD_CLIENT_STATS_INTERVAL_NS)
        {
            print_client_stats(state);
            last_stats_ns = now;
        }

        SDL_Delay(WD_CLIENT_FRAME_DELAY_MS);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    free_cached_cursors();
    SDL_Quit();

    return 0;
}

} // namespace waydisplay
