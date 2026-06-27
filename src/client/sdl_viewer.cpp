#include "sdl_viewer.hpp"

#include "audio_video_sync.h"
#include "client_config_validation.hpp"
#include "client_net.hpp"
#include "client_telemetry.hpp"
#include "content_order.hpp"
#include "render_planning.hpp"
#include "sdl_input.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_input.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace waydisplay {
namespace {

const wd_server_config_payload*      g_client_config = nullptr;
int                                  g_window_width  = 1;
int                                  g_window_height = 1;
SDL_FRect                            g_content_rect{0.0f, 0.0f, 1.0f, 1.0f};
std::array<bool, SDL_SCANCODE_COUNT> g_forwarded_keys{};
bool                                 g_suppress_paste_v_keyup = false;

constexpr uint16_t WD_POINTER_MOD_ALT   = 1u << 0;
constexpr uint16_t WD_POINTER_MOD_SHIFT = 1u << 1;
constexpr uint16_t WD_POINTER_MOD_CTRL  = 1u << 2;
constexpr uint16_t WD_POINTER_MOD_SUPER = 1u << 3;

uint32_t update_audio_video_hold_duration(ClientState& state, bool holding) {
    if (!holding)
    {
        state.stats.audio_video_sync_hold_start_ns.store(0, std::memory_order_relaxed);
        state.stats.audio_video_sync_hold_current_ms.store(0, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t now_ns = wd_now_ns();
    uint64_t start_ns = state.stats.audio_video_sync_hold_start_ns.load(std::memory_order_relaxed);
    if (start_ns == 0)
    {
        state.stats.audio_video_sync_hold_start_ns.compare_exchange_strong(start_ns, now_ns, std::memory_order_relaxed);
        start_ns = state.stats.audio_video_sync_hold_start_ns.load(std::memory_order_relaxed);
    }
    const uint32_t current_ms = now_ns >= start_ns ? static_cast<uint32_t>(std::min<uint64_t>((now_ns - start_ns) / WD_NSEC_PER_MSEC, UINT32_MAX)) : 0;
    state.stats.audio_video_sync_hold_current_ms.store(current_ms, std::memory_order_relaxed);
    uint32_t maximum = state.stats.audio_video_sync_hold_max_ms.load(std::memory_order_relaxed);
    while (current_ms > maximum && !state.stats.audio_video_sync_hold_max_ms.compare_exchange_weak(maximum, current_ms, std::memory_order_relaxed,
                                                                                                    std::memory_order_relaxed)) {}
    return current_ms;
}

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

    uint64_t remaining_ms = (interval_ns - elapsed_ns + WD_NSEC_PER_MSEC - 1ull) / WD_NSEC_PER_MSEC;
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
        SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_TEXT_ENABLED_R, WD_CLIENT_CONTEXT_MENU_TEXT_ENABLED_G,
                               WD_CLIENT_CONTEXT_MENU_TEXT_ENABLED_B, 255);
    }
    else
    {
        SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_TEXT_DISABLED_R, WD_CLIENT_CONTEXT_MENU_TEXT_DISABLED_G,
                               WD_CLIENT_CONTEXT_MENU_TEXT_DISABLED_B, 255);
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

                SDL_FRect px{static_cast<float>(cursor_x + col * scale), static_cast<float>(y + row * scale), static_cast<float>(scale),
                             static_cast<float>(scale)};
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

    SDL_FRect shadow{static_cast<float>(menu.x + WD_CLIENT_CONTEXT_MENU_SHADOW_OFFSET_PX),
                     static_cast<float>(menu.y + WD_CLIENT_CONTEXT_MENU_SHADOW_OFFSET_PX),
                     static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH), static_cast<float>(height)};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, WD_CLIENT_CONTEXT_MENU_SHADOW_ALPHA);
    SDL_RenderFillRect(renderer, &shadow);

    SDL_FRect bg{static_cast<float>(menu.x), static_cast<float>(menu.y), static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH),
                 static_cast<float>(height)};
    SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_BG_R, WD_CLIENT_CONTEXT_MENU_BG_G, WD_CLIENT_CONTEXT_MENU_BG_B,
                           WD_CLIENT_CONTEXT_MENU_BG_ALPHA);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_BORDER_R, WD_CLIENT_CONTEXT_MENU_BORDER_G,
                           WD_CLIENT_CONTEXT_MENU_BORDER_B, 255);
    SDL_RenderRect(renderer, &bg);

    SDL_FRect inner{static_cast<float>(menu.x + 1), static_cast<float>(menu.y + 1), static_cast<float>(WD_CLIENT_CONTEXT_MENU_WIDTH - 2),
                    static_cast<float>(height - 2)};
    SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_INNER_R, WD_CLIENT_CONTEXT_MENU_INNER_G,
                           WD_CLIENT_CONTEXT_MENU_INNER_B, 255);
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
            SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_HOVER_R, WD_CLIENT_CONTEXT_MENU_HOVER_G,
                                   WD_CLIENT_CONTEXT_MENU_HOVER_B, 255);
            SDL_RenderFillRect(renderer, &item_rect);
        }

        if (i == 2)
        {
            SDL_SetRenderDrawColor(renderer, WD_CLIENT_CONTEXT_MENU_SEPARATOR_R, WD_CLIENT_CONTEXT_MENU_SEPARATOR_G,
                                   WD_CLIENT_CONTEXT_MENU_SEPARATOR_B, 255);
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
        state.session.running.store(false, std::memory_order_relaxed);
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

bool send_host_clipboard_to_server(ClientState& state, bool primary, bool force) {
    char* text = primary ? SDL_GetPrimarySelectionText() : SDL_GetClipboardText();
    if (!text)
    {
        return false;
    }

    const ClientSelectionKind kind = primary ? ClientSelectionKind::Primary : ClientSelectionKind::Clipboard;
    const std::string         value(text);
    SDL_free(text);

    if (!client_selection_sync_should_send(state.selection_sync, kind, value, force))
    {
        return true;
    }

    const bool ok = primary ? client_send_primary_text(state, value.c_str()) : client_send_clipboard_text(state, value.c_str());
    if (ok)
    {
        client_selection_sync_note_sent(state.selection_sync, kind, value);
    }
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

    if (have_clipboard && client_selection_sync_should_apply(state.selection_sync, ClientSelectionKind::Clipboard, clipboard))
    {
        if (!SDL_SetClipboardText(clipboard.c_str()))
        {
            log_sdl_warning("SDL_SetClipboardText");
        }
        else
        {
            client_selection_sync_note_applied(state.selection_sync, ClientSelectionKind::Clipboard, clipboard);
        }
    }

    if (have_primary && client_selection_sync_should_apply(state.selection_sync, ClientSelectionKind::Primary, primary))
    {
        if (!SDL_SetPrimarySelectionText(primary.c_str()))
        {
            log_sdl_warning("SDL_SetPrimarySelectionText");
        }
        else
        {
            client_selection_sync_note_applied(state.selection_sync, ClientSelectionKind::Primary, primary);
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








void collect_base_tile_ids_for_rects(const wd_server_config_payload& config, const std::vector<ClientDirtyRect>& rects,
                                     std::vector<uint16_t>& out_tile_ids) {
    out_tile_ids.clear();
    if (config.tile_width == 0 || config.tile_height == 0 || config.total_tiles == 0)
    {
        return;
    }

    for (const ClientDirtyRect& rect : rects)
    {
        if (rect.w == 0 || rect.h == 0 || rect.x >= config.width || rect.y >= config.height)
        {
            continue;
        }
        const uint32_t right  = std::min<uint32_t>(config.width, static_cast<uint32_t>(rect.x) + rect.w);
        const uint32_t bottom = std::min<uint32_t>(config.height, static_cast<uint32_t>(rect.y) + rect.h);
        const uint32_t bx0    = rect.x / config.tile_width;
        const uint32_t by0    = rect.y / config.tile_height;
        const uint32_t bx1    = (right - 1u) / config.tile_width;
        const uint32_t by1    = (bottom - 1u) / config.tile_height;
        for (uint32_t by = by0; by <= by1 && by < config.tiles_y; ++by)
        {
            for (uint32_t bx = bx0; bx <= bx1 && bx < config.tiles_x; ++bx)
            {
                const uint32_t base_id = by * static_cast<uint32_t>(config.tiles_x) + bx;
                if (base_id < config.total_tiles)
                {
                    out_tile_ids.push_back(static_cast<uint16_t>(base_id));
                }
            }
        }
    }
}

uint16_t sdl_button_to_linux_button(uint8_t button) {
    switch (button)
    {
    case SDL_BUTTON_LEFT:
        return WD_INPUT_BUTTON_LEFT;
    case SDL_BUTTON_RIGHT:
        return WD_INPUT_BUTTON_RIGHT;
    case SDL_BUTTON_MIDDLE:
        return WD_INPUT_BUTTON_MIDDLE;
    case SDL_BUTTON_X1:
        return WD_INPUT_BUTTON_SIDE;
    case SDL_BUTTON_X2:
        return WD_INPUT_BUTTON_EXTRA;
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
    const uint32_t mapped  = static_cast<uint32_t>((static_cast<double>(local_x) * g_client_config->width) / g_content_rect.w);

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
    const uint32_t mapped  = static_cast<uint32_t>((static_cast<double>(local_y) * g_client_config->height) / g_content_rect.h);

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

SDL_Texture* create_frame_texture(SDL_Renderer* renderer, uint32_t width, uint32_t height) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width),
                                             static_cast<int>(height));

    if (texture)
    {
        if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST))
        {
            log_sdl_warning("SDL_SetTextureScaleMode");
        }
    }

    return texture;
}

SDL_Texture* create_video_texture(SDL_Renderer* renderer, uint32_t width, uint32_t height) {
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
    if (texture && !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR))
    {
        log_sdl_warning("SDL_SetTextureScaleMode(video)");
    }
    return texture;
}

bool apply_pending_server_config(ClientState& state, SDL_Window* window, SDL_Renderer* renderer, SDL_Texture*& texture,
                                 SDL_Texture*& video_texture, SDL_Texture*& active_texture) {
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

    ClientConfigValidationError config_error{};
    if (!client_normalize_and_validate_server_config(config, &config_error))
    {
        WD_LOG_ERROR("refusing invalid server config update: reason=%s display=%ux%u tile=%ux%u grid=%ux%u total=%u udp=%u",
                     client_config_validation_error_name(config_error), config.width, config.height, config.tile_width, config.tile_height,
                     config.tiles_x, config.tiles_y, config.total_tiles, config.udp_payload_target);
        return false;
    }

    const uint32_t change_flags          = client_classify_server_config_change(state.config, config);
    const bool     transport_changed     = (change_flags & ClientConfigChangeTransport) != 0;
    const bool     stream_reset_required = client_config_change_requires_stream_reset(change_flags);

    if (!stream_reset_required)
    {
        {
            std::lock_guard<std::mutex> lock(state.config_mutex);
            if (state.media_clock_id != config.media_clock_id)
            {
                state.media_clock_id              = config.media_clock_id;
                state.media_clock_local_origin_ns = wd_now_ns();
            }
            state.config = config;
        }
        if (!client_send_config_applied(state, config.session_id, config.config_epoch))
        {
            WD_LOG_ERROR("failed to acknowledge lightweight server config session=%u", config.session_id);
            state.session.running.store(false, std::memory_order_relaxed);
            return false;
        }
        WD_LOG_DEBUG("acknowledged lightweight server config session=%u changes=0x%x", config.session_id, change_flags);
        return false;
    }

    WD_LOG_INFO("server config updated: session=%u display=%ux%u tile=%ux%u grid=%ux%u total=%u changes=0x%x", config.session_id,
                config.width, config.height, config.tile_width, config.tile_height, config.tiles_x, config.tiles_y, config.total_tiles,
                change_flags);

    std::vector<uint32_t> new_framebuffer(static_cast<size_t>(config.width) * config.height, WD_CLIENT_FRAMEBUFFER_CLEAR_XRGB);
    std::vector<uint64_t>                   new_received_generation(config.total_tiles, 0);
    std::vector<uint64_t>                   new_presented_generation(config.total_tiles, 0);
    std::vector<uint64_t>                   new_pending_present_generation(config.total_tiles, 0);
    std::vector<ClientPendingTileTelemetry> new_pending_tile_telemetry(config.total_tiles);
    std::vector<uint64_t>                   new_retx_queued_generation(config.total_tiles, 0);
    std::vector<uint64_t>                   new_retx_last_requested_generation(config.total_tiles, 0);
    std::vector<uint64_t>                   new_retx_last_request_ns(config.total_tiles, 0);
    std::vector<uint64_t>                   new_retx_inflight_generation(config.total_tiles, 0);
    std::vector<uint64_t>                   new_retx_inflight_since_ns(config.total_tiles, 0);
    std::vector<uint64_t>                   new_retx_summary_pending_generation(config.total_tiles, 0);
    std::vector<uint64_t>                   new_retx_summary_pending_since_ns(config.total_tiles, 0);
    std::vector<uint32_t>                   new_retx_summary_pending_position(config.total_tiles, UINT32_MAX);
    std::vector<uint8_t> new_udp_recv_buffer(
        WD_UDP_TILE_HEADER_MAX_SIZE + config.udp_payload_target + WD_CLIENT_UDP_RECV_SLACK_BYTES, 0);
    ClientDirtyTileGrid                     new_dirty_tiles;
    if (!configure_client_dirty_tile_grid(new_dirty_tiles, config.width, config.height, config.tile_width, config.tile_height))
    {
        WD_LOG_ERROR("failed to configure client dirty tile grid");
        state.session.running.store(false, std::memory_order_relaxed);
        return false;
    }

    SDL_Texture* new_texture       = create_frame_texture(renderer, config.width, config.height);
    SDL_Texture* new_video_texture = create_video_texture(renderer, config.width, config.height);

    if (!new_texture || !new_video_texture)
    {
        SDL_DestroyTexture(new_texture);
        SDL_DestroyTexture(new_video_texture);
        WD_LOG_ERROR("SDL_CreateTexture after resize failed: %s", SDL_GetError());
        state.session.running.store(false, std::memory_order_relaxed);
        return false;
    }

    {
        std::lock_guard<std::mutex> processing_lock(state.udp_processing_mutex);
        if (transport_changed && !client_reconfigure_udp_transport_locked(state, config))
        {
            SDL_DestroyTexture(new_texture);
            SDL_DestroyTexture(new_video_texture);
            WD_LOG_ERROR("failed to apply UDP transport update for session=%u", config.session_id);
            state.session.running.store(false, std::memory_order_relaxed);
            return false;
        }
        std::scoped_lock reconfigure_state_lock(state.config_mutex, state.framebuffer_mutex, state.dirty_rect_mutex,
                                                  state.generation_mutex, state.retx_mutex);

        if (state.media_clock_id != config.media_clock_id)
        {
            state.media_clock_id              = config.media_clock_id;
            state.media_clock_local_origin_ns = wd_now_ns();
        }
        state.config      = config;
        state.framebuffer = std::move(new_framebuffer);
        {
            std::lock_guard<std::mutex> video_lock(state.video_frame_mutex);
            state.video_present_queue.clear();
            state.pending_video_frame_dirty.store(false, std::memory_order_release);
        }
        const uint64_t tile_epoch = wd_client_stream_ownership_reset_to_tiles(&state.stream_ownership);
        state.pending_dirty_tiles = std::move(new_dirty_tiles);
        state.pending_dirty_rect_count.store(0, std::memory_order_release);
        state.pending_dirty_epoch        = tile_epoch;
        state.received_generation        = std::move(new_received_generation);
        state.presented_generation       = std::move(new_presented_generation);
        state.pending_present_generation = std::move(new_pending_present_generation);
        {
            std::lock_guard<std::mutex> present_lock(state.present_mutex);
            state.pending_tile_telemetry = std::move(new_pending_tile_telemetry);
        }
        state.retx_queue.clear();
        state.retx_queued_generation          = std::move(new_retx_queued_generation);
        state.retx_last_requested_generation  = std::move(new_retx_last_requested_generation);
        state.retx_last_request_ns            = std::move(new_retx_last_request_ns);
        state.retx_inflight_generation        = std::move(new_retx_inflight_generation);
        state.retx_inflight_since_ns          = std::move(new_retx_inflight_since_ns);
        state.retx_summary_pending_generation = std::move(new_retx_summary_pending_generation);
        state.retx_summary_pending_since_ns   = std::move(new_retx_summary_pending_since_ns);
        state.retx_summary_pending_tiles.clear();
        state.retx_summary_pending_position      = std::move(new_retx_summary_pending_position);
        state.retx_summary_pending_count         = 0;
        state.next_summary_promote_ns            = 0;
        state.summary_large_repair_not_before_ns = 0;
        state.summary_repair_loss_signal_until_ns.store(0, std::memory_order_relaxed);
        state.udp_recv_buffer = std::move(new_udp_recv_buffer);
        state.client_config_generation.fetch_add(1, std::memory_order_release);
    }

    client_reset_content_epoch(state, config.content_epoch, WD_CLIENT_CONTENT_OWNER_TILES);

    SDL_DestroyTexture(texture);
    SDL_DestroyTexture(video_texture);
    texture        = new_texture;
    video_texture  = new_video_texture;
    active_texture = texture;

    g_client_config = &state.config;
    if (!SDL_SetWindowMinimumSize(window, WD_CLIENT_MIN_WINDOW_WIDTH, WD_CLIENT_MIN_WINDOW_HEIGHT))
    {
        log_sdl_warning("SDL_SetWindowMinimumSize");
    }
    update_window_size(window);

    if (!client_send_config_applied(state, config.session_id, config.config_epoch))
    {
        WD_LOG_ERROR("failed to acknowledge applied server config session=%u", config.session_id);
        state.session.running.store(false, std::memory_order_relaxed);
        return false;
    }
    WD_LOG_DEBUG("acknowledged applied server config session=%u", config.session_id);

    return true;
}

bool upload_argb_texture_locked(SDL_Texture* texture, const SDL_Rect* rect, const uint32_t* source, int source_pitch, uint32_t width,
                                uint32_t height) {
    if (!texture || !source || width == 0 || height == 0 || source_pitch <= 0)
    {
        return false;
    }

    void* locked_pixels = nullptr;
    int   locked_pitch  = 0;
    if (!SDL_LockTexture(texture, rect, &locked_pixels, &locked_pitch))
    {
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(width) * WD_BYTES_PER_PIXEL;
    if (locked_pitch < 0 || static_cast<size_t>(locked_pitch) < row_bytes || static_cast<size_t>(source_pitch) < row_bytes)
    {
        SDL_UnlockTexture(texture);
        return false;
    }

    const auto* src = reinterpret_cast<const uint8_t*>(source);
    auto*       dst = static_cast<uint8_t*>(locked_pixels);
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(dst + static_cast<size_t>(y) * locked_pitch, src + static_cast<size_t>(y) * source_pitch, row_bytes);
    }

    SDL_UnlockTexture(texture);
    return true;
}

bool update_argb_texture(SDL_Texture* texture, const SDL_Rect& rect, const uint32_t* source, int source_pitch, uint32_t width,
                         uint32_t height) {
    if (!texture || !source || width == 0 || height == 0 || source_pitch <= 0)
    {
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(width) * WD_BYTES_PER_PIXEL;
    if (static_cast<size_t>(source_pitch) < row_bytes)
    {
        return false;
    }

    return SDL_UpdateTexture(texture, &rect, source, source_pitch);
}

void record_texture_upload_stats(ClientState& state, uint64_t started_ns, uint64_t pixels) {
    const uint64_t elapsed_ns = wd_now_ns() - started_ns;
    state.stats.sdl_texture_upload_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_texture_upload_sum_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    state.stats.sdl_texture_upload_pixels.fetch_add(pixels, std::memory_order_relaxed);
    record_atomic_max(state.stats.sdl_texture_upload_max_ns, elapsed_ns);
}

void record_texture_call_cost(ClientState& state, ClientTextureUploadCostModel& costs, bool texture_lock, uint64_t started_ns,
                              uint64_t pixels) {
    observe_texture_upload_call(costs, texture_lock, wd_now_ns() - started_ns, pixels);
    state.stats.sdl_texture_model_update_call_ns.store(costs.update_call_cost_ns, std::memory_order_relaxed);
    state.stats.sdl_texture_model_lock_call_ns.store(costs.lock_call_cost_ns, std::memory_order_relaxed);
    state.stats.sdl_texture_model_pixel_cost_q16.store(costs.pixel_cost_q16, std::memory_order_relaxed);
}

void record_framebuffer_snapshot_stats(ClientState& state, ClientTextureUploadCostModel& costs, uint64_t started_ns, uint64_t pixels) {
    const uint64_t elapsed_ns = wd_now_ns() - started_ns;
    observe_framebuffer_snapshot(costs, elapsed_ns, pixels);
    state.stats.framebuffer_snapshot_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.framebuffer_snapshot_sum_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    state.stats.framebuffer_snapshot_pixels.fetch_add(pixels, std::memory_order_relaxed);
    record_atomic_max(state.stats.framebuffer_snapshot_max_ns, elapsed_ns);
}

void record_framebuffer_lock_stats(ClientState& state, uint64_t wait_ns, uint64_t hold_ns) {
    state.stats.framebuffer_lock_wait_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.framebuffer_lock_wait_sum_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    record_atomic_max(state.stats.framebuffer_lock_wait_max_ns, wait_ns);
    state.stats.framebuffer_lock_hold_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.framebuffer_lock_hold_sum_ns.fetch_add(hold_ns, std::memory_order_relaxed);
    record_atomic_max(state.stats.framebuffer_lock_hold_max_ns, hold_ns);
    const uint64_t old_ewma = state.framebuffer_lock_wait_ewma_ns.load(std::memory_order_relaxed);
    const uint64_t next_ewma =
        old_ewma == 0 ? wait_ns
                      : (old_ewma * WD_CLIENT_FRAMEBUFFER_LOCK_EWMA_OLD_NUMERATOR + wait_ns) / WD_CLIENT_FRAMEBUFFER_LOCK_EWMA_DENOMINATOR;
    state.framebuffer_lock_wait_ewma_ns.store(next_ewma, std::memory_order_relaxed);
}

bool snapshot_full_framebuffer(ClientState& state, uint32_t frame_width, uint32_t frame_height, std::vector<uint32_t>& staging,
                               ClientTextureUploadCostModel& costs) {
    const size_t expected_pixels = static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height);
    if (frame_width == 0 || frame_height == 0 || expected_pixels == 0)
    {
        return false;
    }
    staging.resize(expected_pixels);
    const uint64_t               started_ns      = wd_now_ns();
    const uint64_t               lock_started_ns = wd_now_ns();
    std::unique_lock<std::mutex> framebuffer_lock(state.framebuffer_mutex);
    const uint64_t               lock_acquired_ns = wd_now_ns();
    if (state.framebuffer.size() < expected_pixels)
    {
        return false;
    }
    std::memcpy(staging.data(), state.framebuffer.data(), expected_pixels * sizeof(uint32_t));
    const uint64_t lock_released_ns = wd_now_ns();
    framebuffer_lock.unlock();
    record_framebuffer_lock_stats(state, lock_acquired_ns - lock_started_ns, lock_released_ns - lock_acquired_ns);
    record_framebuffer_snapshot_stats(state, costs, started_ns, expected_pixels);
    return true;
}

bool upload_full_texture(ClientState& state, SDL_Texture* texture, uint32_t frame_width, uint32_t frame_height,
                         std::vector<uint32_t>& staging, ClientTextureUploadCostModel& costs) {
    const uint64_t started_ns = wd_now_ns();
    if (!snapshot_full_framebuffer(state, frame_width, frame_height, staging, costs))
    {
        return false;
    }
    const uint64_t call_started_ns = wd_now_ns();
    const bool     ok = upload_argb_texture_locked(texture, nullptr, staging.data(), static_cast<int>(frame_width * WD_BYTES_PER_PIXEL),
                                                   frame_width, frame_height);
    record_texture_call_cost(state, costs, true, call_started_ns, static_cast<uint64_t>(frame_width) * frame_height);
    if (ok)
    {
        state.stats.framebuffer_staged_uploads.fetch_add(1, std::memory_order_relaxed);
        state.stats.sdl_texture_full_uploads.fetch_add(1, std::memory_order_relaxed);
        state.stats.sdl_texture_lock_calls.fetch_add(1, std::memory_order_relaxed);
        record_texture_upload_stats(state, started_ns, static_cast<uint64_t>(frame_width) * frame_height);
    }
    return ok;
}

struct VideoPresentInfo {
    bool     valid          = false;
    uint64_t frame_id       = 0;
    uint64_t pts_usec       = 0;
    uint64_t epoch          = 0;
    uint64_t content_epoch  = 0;
    uint32_t retry_after_ms = 0;
};

enum class VideoTextureUploadResult : uint8_t {
    NoFrame,
    Held,
    AudioDropped,
    Uploaded,
    Stale,
    UploadedStale,
    Failed,
};

VideoTextureUploadResult upload_pending_video_texture(ClientState& state, SDL_Texture* texture, uint32_t frame_width, uint32_t frame_height,
                                                      ClientVideoFrameBuffer& frame, VideoPresentInfo& present_info) {
    const uint64_t started_ns        = wd_now_ns();
    uint64_t       frame_epoch       = 0;
    bool           dropped_for_audio = false;
    {
        std::scoped_lock dirty_video_lock(state.dirty_rect_mutex, state.video_frame_mutex);
        if (!state.pending_video_frame_dirty.load(std::memory_order_acquire))
        {
            return VideoTextureUploadResult::NoFrame;
        }

        for (;;)
        {
            const ClientQueuedVideoFrame* queued = state.video_present_queue.front();
            if (!queued)
            {
                state.pending_video_frame_dirty.store(false, std::memory_order_release);
                state.stats.audio_video_delta_samples.store(0, std::memory_order_relaxed);
                return dropped_for_audio ? VideoTextureUploadResult::AudioDropped : VideoTextureUploadResult::NoFrame;
            }
            if (queued->width != frame_width || queued->height != frame_height)
            {
                return VideoTextureUploadResult::Failed;
            }

            uint64_t audio_playhead_samples = 0;
            uint32_t audio_startup_hold_ms   = 0;
            bool     audio_startup_timed_out = false;
            const bool audio_waiting = client_audio_playback_video_gate(state.session.audio_playback, wd_now_ns(),
                                                                         &audio_startup_hold_ms, &audio_startup_timed_out);
            state.stats.audio_video_startup_hold_ms.store(audio_waiting ? audio_startup_hold_ms : 0, std::memory_order_relaxed);
            state.stats.audio_playback_state.store(client_audio_playback_state(state.session.audio_playback),
                                                   std::memory_order_relaxed);
            if (audio_startup_timed_out)
            {
                state.stats.audio_video_startup_timeouts.fetch_add(1, std::memory_order_relaxed);
                WD_LOG_WARN("audio startup did not establish a presentation clock within %u ms; presenting video without audio sync",
                            WD_CLIENT_AUDIO_VIDEO_STARTUP_HOLD_MAX_MS);
            }
            if (client_audio_playback_playhead_samples(state.session.audio_playback, &audio_playhead_samples))
            {
                const struct wd_client_audio_video_sync_plan sync =
                    wd_client_audio_video_sync_plan_compute(queued->pts_usec, audio_playhead_samples, WD_AUDIO_SAMPLE_RATE_DEFAULT);
                state.stats.audio_video_delta_samples.store(sync.delta_samples, std::memory_order_relaxed);
                if (sync.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD)
                {
                    const uint32_t hold_ms = update_audio_video_hold_duration(state, true);
                    state.stats.audio_video_sync_holds.fetch_add(1, std::memory_order_relaxed);
                    if (hold_ms <= WD_CLIENT_AUDIO_VIDEO_PLAYING_HOLD_MAX_MS)
                    {
                        present_info.retry_after_ms = sync.retry_after_ms;
                        return VideoTextureUploadResult::Held;
                    }
                    WD_LOG_WARN("audio/video sync hold exceeded %u ms; temporarily presenting video without audio clock",
                                WD_CLIENT_AUDIO_VIDEO_PLAYING_HOLD_MAX_MS);
                    update_audio_video_hold_duration(state, false);
                }
                if (sync.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_DROP)
                {
                    ClientQueuedVideoFrame dropped = state.video_present_queue.pop_front();
                    state.video_present_queue.recycle(std::move(dropped.buffer));
                    state.stats.audio_video_sync_drops.fetch_add(1, std::memory_order_relaxed);
                    dropped_for_audio = true;
                    state.pending_video_frame_dirty.store(!state.video_present_queue.empty(), std::memory_order_release);
                    continue;
                }
            }
            else if (audio_waiting)
            {
                /* Hold only while an audio epoch is establishing its initial
                 * jitter buffer. After starvation, audio relinquishes the
                 * master clock until playback has rebuffered. */
                const uint32_t hold_ms = update_audio_video_hold_duration(state, true);
                state.stats.audio_video_sync_holds.fetch_add(1, std::memory_order_relaxed);
                if (hold_ms <= WD_CLIENT_AUDIO_VIDEO_PLAYING_HOLD_MAX_MS)
                {
                    present_info.retry_after_ms = WD_CLIENT_FRAME_DELAY_MS;
                    return VideoTextureUploadResult::Held;
                }
                update_audio_video_hold_duration(state, false);
            }

            update_audio_video_hold_duration(state, false);
            ClientQueuedVideoFrame selected = state.video_present_queue.pop_front();
            std::swap(frame, selected.buffer);
            state.video_present_queue.recycle(std::move(selected.buffer));
            present_info.frame_id = selected.frame_id;
            present_info.pts_usec = selected.pts_usec;
            frame_epoch           = selected.epoch;
            present_info.epoch    = frame_epoch;
            {
                std::lock_guard<std::mutex> content_lock(state.remote_content_mutex);
                present_info.content_epoch = state.remote_content_owner == WD_CLIENT_CONTENT_OWNER_VIDEO ? state.remote_content_epoch : 0;
            }
            state.pending_video_frame_dirty.store(!state.video_present_queue.empty(), std::memory_order_release);
            break;
        }
    }

    if (!wd_client_stream_ownership_is_current(&state.stream_ownership, frame_epoch, WD_CLIENT_CONTENT_OWNER_VIDEO))
    {
        return VideoTextureUploadResult::Stale;
    }

    const size_t expected_pixels = static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height);
    if (frame_width == 0 || frame_height == 0 || !frame.valid() || frame.width != frame_width || frame.height != frame_height ||
        frame.u_offset >= frame.bytes.size() || frame.v_offset >= frame.bytes.size())
    {
        return VideoTextureUploadResult::Failed;
    }

    const uint8_t* y = frame.bytes.data();
    const uint8_t* u = frame.bytes.data() + frame.u_offset;
    const uint8_t* v = frame.bytes.data() + frame.v_offset;
    if (!SDL_UpdateYUVTexture(texture, nullptr, y, static_cast<int>(frame.y_pitch), u, static_cast<int>(frame.uv_pitch), v,
                              static_cast<int>(frame.uv_pitch)))
    {
        return VideoTextureUploadResult::Failed;
    }

    state.stats.sdl_texture_full_uploads.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_video_texture_uploads.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_video_texture_upload_pixels.fetch_add(expected_pixels, std::memory_order_relaxed);
    record_texture_upload_stats(state, started_ns, expected_pixels);

    if (!wd_client_stream_ownership_is_current(&state.stream_ownership, frame_epoch, WD_CLIENT_CONTENT_OWNER_VIDEO))
    {
        return VideoTextureUploadResult::UploadedStale;
    }

    present_info.valid = true;
    return VideoTextureUploadResult::Uploaded;
}

struct StagedTextureRect {
    SDL_Rect rect{};
    size_t   pixel_offset     = 0;
    uint32_t width            = 0;
    uint32_t height           = 0;
    bool     use_texture_lock = false;
};

bool upload_dirty_texture_rects_staged(ClientState& state, SDL_Texture* texture, const std::vector<ClientDirtyRect>& rects,
                                       const DirtyTextureUploadPlan& plan, uint32_t frame_width, uint32_t frame_height,
                                       std::vector<uint32_t>& staging, std::vector<StagedTextureRect>& staged_rects,
                                       ClientTextureUploadCostModel& costs) {
    const uint64_t started_ns = wd_now_ns();
    staged_rects.clear();
    uint64_t total_pixels = 0;

    const auto add_rect = [&](const ClientDirtyRect& rect, bool use_texture_lock) {
        if (rect.w == 0 || rect.h == 0 || rect.x >= frame_width || rect.y >= frame_height)
        {
            return true;
        }
        const uint32_t width  = std::min<uint32_t>(rect.w, frame_width - rect.x);
        const uint32_t height = std::min<uint32_t>(rect.h, frame_height - rect.y);
        if (width == 0 || height == 0)
        {
            return true;
        }
        const uint64_t pixels = static_cast<uint64_t>(width) * height;
        if (pixels > SIZE_MAX || total_pixels > SIZE_MAX - pixels)
        {
            return false;
        }
        StagedTextureRect staged{};
        staged.rect             = {static_cast<int>(rect.x), static_cast<int>(rect.y), static_cast<int>(width), static_cast<int>(height)};
        staged.pixel_offset     = static_cast<size_t>(total_pixels);
        staged.width            = width;
        staged.height           = height;
        staged.use_texture_lock = use_texture_lock;
        staged_rects.push_back(staged);
        total_pixels += pixels;
        return true;
    };

    staged_rects.reserve(plan.mode == DirtyTextureUploadMode::Bounds ? 1u : rects.size());
    if (plan.mode == DirtyTextureUploadMode::Bounds)
    {
        if (!add_rect(plan.bounds, true))
        {
            return false;
        }
    }
    else
    {
        for (const ClientDirtyRect& rect : rects)
        {
            if (!add_rect(rect, false))
            {
                return false;
            }
        }
    }
    staging.resize(static_cast<size_t>(total_pixels));

    const uint64_t               snapshot_started_ns = wd_now_ns();
    const uint64_t               lock_started_ns     = wd_now_ns();
    std::unique_lock<std::mutex> framebuffer_lock(state.framebuffer_mutex);
    const uint64_t               lock_acquired_ns = wd_now_ns();
    const size_t                 expected_pixels  = static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height);
    if (frame_width == 0 || frame_height == 0 || state.framebuffer.size() < expected_pixels)
    {
        return false;
    }
    for (const StagedTextureRect& staged : staged_rects)
    {
        uint32_t* destination = staging.data() + staged.pixel_offset;
        for (uint32_t row = 0; row < staged.height; ++row)
        {
            const uint32_t* source = state.framebuffer.data() + static_cast<size_t>(staged.rect.y + static_cast<int>(row)) * frame_width +
                                     static_cast<size_t>(staged.rect.x);
            std::memcpy(destination + static_cast<size_t>(row) * staged.width, source,
                        static_cast<size_t>(staged.width) * sizeof(uint32_t));
        }
    }
    const uint64_t lock_released_ns = wd_now_ns();
    framebuffer_lock.unlock();
    record_framebuffer_lock_stats(state, lock_acquired_ns - lock_started_ns, lock_released_ns - lock_acquired_ns);
    record_framebuffer_snapshot_stats(state, costs, snapshot_started_ns, total_pixels);

    uint64_t texture_lock_calls   = 0;
    uint64_t texture_update_calls = 0;
    for (const StagedTextureRect& staged : staged_rects)
    {
        const uint32_t* source          = staging.data() + staged.pixel_offset;
        const int       source_pitch    = static_cast<int>(staged.width * WD_BYTES_PER_PIXEL);
        const uint64_t  call_started_ns = wd_now_ns();
        const bool uploaded = staged.use_texture_lock
                                  ? upload_argb_texture_locked(texture, &staged.rect, source, source_pitch, staged.width, staged.height)
                                  : update_argb_texture(texture, staged.rect, source, source_pitch, staged.width, staged.height);
        record_texture_call_cost(state, costs, staged.use_texture_lock, call_started_ns,
                                 static_cast<uint64_t>(staged.width) * staged.height);
        if (!uploaded)
        {
            return false;
        }
        if (staged.use_texture_lock)
        {
            ++texture_lock_calls;
        }
        else
        {
            ++texture_update_calls;
        }
    }

    state.stats.framebuffer_staged_uploads.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_texture_partial_uploads.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_texture_dirty_rects.fetch_add(staged_rects.size(), std::memory_order_relaxed);
    state.stats.sdl_texture_lock_calls.fetch_add(texture_lock_calls, std::memory_order_relaxed);
    state.stats.sdl_texture_update_calls.fetch_add(texture_update_calls, std::memory_order_relaxed);
    record_texture_upload_stats(state, started_ns, total_pixels);
    return true;
}

bool upload_dirty_texture_rects(ClientState& state, SDL_Texture* texture, const std::vector<ClientDirtyRect>& rects,
                                const DirtyTextureUploadPlan& plan, uint32_t frame_width, uint32_t frame_height,
                                std::vector<uint32_t>& staging, std::vector<StagedTextureRect>& staged_rects,
                                ClientTextureUploadCostModel& costs) {
    /* Never call into SDL while the network producer's framebuffer mutex is
     * held. Even a small SDL_UpdateTexture() can block in a driver or backend.
     * Snapshot the selected pixels first, then perform all renderer calls after
     * releasing the model lock. */
    return upload_dirty_texture_rects_staged(state, texture, rects, plan, frame_width, frame_height, staging, staged_rects, costs);
}

bool client_send_keyboard_key_checked(ClientState& state, uint16_t evdev_key_code, bool pressed) {
    if (client_send_keyboard_key(state, evdev_key_code, pressed))
    {
        return true;
    }

    WD_LOG_ERROR("failed to send keyboard event evdev=%u pressed=%u", evdev_key_code, pressed ? 1 : 0);
    state.session.running.store(false, std::memory_order_relaxed);
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

    if (event.type == SDL_EVENT_CLIPBOARD_UPDATE)
    {
        if (!event.clipboard.owner && !send_host_clipboard_to_server(state, false, false))
        {
            WD_LOG_WARN("failed to synchronize host clipboard update");
        }
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
                send_host_clipboard_to_server(state, false, true);
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
        pointer.connection_token    = state.config.connection_token;
        pointer.client_timestamp_ns = wd_now_ns();
        pointer.event_type          = WD_POINTER_EVENT_MOTION;
        pointer.x                   = map_mouse_coord_x(event.motion.x);
        pointer.y                   = map_mouse_coord_y(event.motion.y);
        pointer.modifiers           = current_pointer_modifiers();

        if (!client_send_pointer_event(state, pointer))
        {
            WD_LOG_ERROR("failed to send pointer motion");
            state.session.running.store(false, std::memory_order_relaxed);
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
        pointer.connection_token    = state.config.connection_token;
        pointer.client_timestamp_ns = wd_now_ns();
        pointer.event_type          = WD_POINTER_EVENT_BUTTON;
        pointer.x                   = map_mouse_coord_x(event.button.x);
        pointer.y                   = map_mouse_coord_y(event.button.y);
        pointer.button              = linux_button;
        pointer.button_state        = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? WD_POINTER_BUTTON_PRESSED : WD_POINTER_BUTTON_RELEASED;
        pointer.modifiers           = current_pointer_modifiers();

        if (linux_button == WD_INPUT_BUTTON_MIDDLE && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            /*
             * Publish the host clipboard as primary selection first, then forward
             * the middle click so the Wayland client performs its normal primary
             * paste request.
             */
            send_host_clipboard_to_server(state, true, true);
        }

        if (!client_send_pointer_event(state, pointer))
        {
            WD_LOG_ERROR("failed to send pointer button");
            state.session.running.store(false, std::memory_order_relaxed);
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
            pointer.connection_token    = state.config.connection_token;
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
                state.session.running.store(false, std::memory_order_relaxed);
            }
        }

        if (event.wheel.x != 0)
        {
            wd_pointer_event_payload pointer{};
            pointer.session_id          = state.config.session_id;
            pointer.connection_token    = state.config.connection_token;
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
                state.session.running.store(false, std::memory_order_relaxed);
            }
        }

        return;
    }
}

bool present_sdl_frame(ClientState& state, SDL_Renderer* renderer, SDL_Texture* texture, const ContextMenu& context_menu,
                       bool remote_texture_updated, const VideoPresentInfo& video_present,
                       const ClientPresentTelemetryBatch& tile_telemetry, uint64_t& last_present_ns) {
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
        if (!video_present.valid)
        {
            state.stats.tile_frames_presented.fetch_add(1, std::memory_order_relaxed);
            if (tile_telemetry.content_epoch != 0)
            {
                record_atomic_max(state.stats.tile_content_epoch_presented, tile_telemetry.content_epoch);
            }
        }
    }
    state.stats.sdl_present_samples.fetch_add(1, std::memory_order_relaxed);
    state.stats.sdl_present_sum_ns.fetch_add(present_elapsed_ns, std::memory_order_relaxed);
    record_atomic_max(state.stats.sdl_present_max_ns, present_elapsed_ns);

    const uint64_t present_ns = wd_now_ns();
    last_present_ns           = present_ns;

    if (video_present.valid)
    {
        state.stats.video_frames_presented.fetch_add(1, std::memory_order_relaxed);
        state.stats.video_last_frame_id_presented.store(video_present.frame_id, std::memory_order_relaxed);
        if (video_present.content_epoch != 0)
        {
            record_atomic_max(state.stats.video_content_epoch_presented, video_present.content_epoch);
        }
        const uint64_t pts_ns = wd_media_local_deadline_ns(state.media_clock_local_origin_ns, video_present.pts_usec);
        if (state.media_clock_local_origin_ns != 0 && present_ns >= pts_ns)
        {
            state.stats.video_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.video_present_latency_sum_ns.fetch_add(present_ns - pts_ns, std::memory_order_relaxed);
        }
    }

    if (tile_telemetry.completion_count != 0 && present_ns >= tile_telemetry.claimed_ns)
    {
        state.stats.tile_present_latency_samples.fetch_add(tile_telemetry.completion_count, std::memory_order_relaxed);
        const uint64_t present_after_claim_ns = present_ns - tile_telemetry.claimed_ns;
        const uint64_t latency_sum = tile_telemetry.completion_age_sum_ns + present_after_claim_ns * tile_telemetry.completion_count;
        state.stats.tile_present_latency_sum_ns.fetch_add(latency_sum, std::memory_order_relaxed);
    }
    for (uint8_t i = 0; i < tile_telemetry.input_sequence_count; ++i)
    {
        const uint64_t sequence           = tile_telemetry.input_sequences[i];
        uint64_t       input_timestamp_ns = 0;
        if (take_input_timestamp(state, sequence, input_timestamp_ns) && present_ns >= input_timestamp_ns)
        {
            state.stats.input_sequence_present_latency_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.input_sequence_present_latency_sum_ns.fetch_add(present_ns - input_timestamp_ns, std::memory_order_relaxed);
        }
    }

    const uint64_t input_timestamp_ns =
        remote_texture_updated ? state.stats.latest_input_event_timestamp_ns.exchange(0, std::memory_order_relaxed) : 0;
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

    SDL_Window* window =
        SDL_CreateWindow("WayDisplay Client", state.config.width, state.config.height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);

    if (!window)
    {
        WD_LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
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
        return 1;
    }

    const char* renderer_name = SDL_GetRendererName(renderer);
    WD_LOG_INFO("SDL renderer: requested=%s active=%s", renderer_driver, renderer_name ? renderer_name : "unknown");

    if (!SDL_SetRenderVSync(renderer, state.stream_config.disable_vsync ? SDL_RENDERER_VSYNC_DISABLED : 1))
    {
        WD_LOG_ERROR("SDL_SetRenderVSync failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return 1;
    }

    if (!SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND))
    {
        WD_LOG_ERROR("SDL_SetRenderDrawBlendMode failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return 1;
    }

    SDL_Texture* texture        = create_frame_texture(renderer, state.config.width, state.config.height);
    SDL_Texture* video_texture  = create_video_texture(renderer, state.config.width, state.config.height);
    SDL_Texture* active_texture = texture;

    if (!texture || !video_texture)
    {
        WD_LOG_ERROR("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyTexture(texture);
        SDL_DestroyTexture(video_texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return 1;
    }

    uint64_t                                last_stats_ns             = wd_now_ns();
    uint64_t                                last_stats_log_ns         = last_stats_ns;
    bool                                    frame_dirty               = true;
    bool                                    texture_needs_full_upload = true;
    uint16_t                                last_requested_width      = state.config.width;
    uint16_t                                last_requested_height     = state.config.height;
    uint16_t                                pending_resize_width      = 0;
    uint16_t                                pending_resize_height     = 0;
    uint64_t                                pending_resize_since_ns   = 0;
    uint64_t                                last_present_ns           = 0;
    ContextMenu                             context_menu;
    ClientPresentTelemetryBatch             tile_telemetry_batch;
    std::vector<ClientDirtyRect>            dirty_rects;
    std::vector<uint16_t>                   dirty_tile_ids;
    std::vector<ClientTileGenerationUpdate> tile_generation_updates;
    ClientVideoFrameBuffer                  video_upload_frame;
    std::vector<uint32_t>                   tile_upload_pixels;
    std::vector<StagedTextureRect>          staged_texture_rects;
    ClientTextureUploadCostModel            texture_upload_costs;

    while (state.session.running.load(std::memory_order_relaxed))
    {
        const uint64_t render_wake_sequence = state.render_wake.sequence();
        apply_pending_cursor_shape(state);
        drain_remote_selection_updates(state);

        if (apply_pending_server_config(state, window, renderer, texture, video_texture, active_texture))
        {
            last_requested_width      = state.config.width;
            last_requested_height     = state.config.height;
            pending_resize_width      = 0;
            pending_resize_height     = 0;
            pending_resize_since_ns   = 0;
            frame_dirty               = true;
            texture_needs_full_upload = true;
        }

        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                release_forwarded_keyboard_keys(state);
                state.session.running.store(false, std::memory_order_relaxed);
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

        const bool local_frame_dirty      = frame_dirty || texture_needs_full_upload;
        bool       remote_frame_dirty     = false;
        bool       remote_texture_updated = false;
        uint32_t   video_hold_wait_ms     = 0;

        const bool video_frame_pending  = state.pending_video_frame_dirty.load(std::memory_order_acquire);
        const bool remote_dirty_pending = video_frame_pending || state.pending_dirty_rect_count.load(std::memory_order_acquire) != 0;
        if (local_frame_dirty || remote_dirty_pending)
        {
            const uint64_t present_check_ns = wd_now_ns();
            remote_frame_dirty =
                remote_dirty_pending && (local_frame_dirty || client_local_present_due(state, last_present_ns, present_check_ns));
        }

        if (local_frame_dirty || remote_frame_dirty)
        {
            const uint32_t frame_width  = state.config.width;
            const uint32_t frame_height = state.config.height;

            VideoPresentInfo video_present;
            tile_generation_updates.clear();
            struct wd_client_content_ownership_snapshot uploaded_ownership{};
            bool                           content_texture_updated = false;
            bool                           upload_has_ownership    = false;
            if (texture_needs_full_upload || remote_frame_dirty)
            {
                dirty_rects.clear();
                VideoTextureUploadResult video_result = VideoTextureUploadResult::NoFrame;
                if (state.pending_video_frame_dirty.load(std::memory_order_acquire))
                {
                    video_result =
                        upload_pending_video_texture(state, video_texture, frame_width, frame_height, video_upload_frame, video_present);
                    if (video_result == VideoTextureUploadResult::Failed)
                    {
                        WD_LOG_ERROR("failed to update SDL video texture: %s", SDL_GetError());
                        state.session.running.store(false, std::memory_order_relaxed);
                        break;
                    }
                }

                if (video_result == VideoTextureUploadResult::Held || video_result == VideoTextureUploadResult::AudioDropped)
                {
                    remote_frame_dirty = false;
                    if (video_result == VideoTextureUploadResult::Held)
                    {
                        video_hold_wait_ms = video_present.retry_after_ms != 0 ? video_present.retry_after_ms : WD_CLIENT_FRAME_DELAY_MS;
                    }
                }
                else if (video_result == VideoTextureUploadResult::Uploaded)
                {
                    active_texture          = video_texture;
                    uploaded_ownership      = {video_present.epoch, WD_CLIENT_CONTENT_OWNER_VIDEO};
                    upload_has_ownership    = true;
                    content_texture_updated = true;
                    remote_texture_updated  = true;
                }
                else
                {
                    const bool video_result_stale =
                        video_result == VideoTextureUploadResult::Stale || video_result == VideoTextureUploadResult::UploadedStale;
                    if (video_result == VideoTextureUploadResult::UploadedStale)
                    {
                        uploaded_ownership      = {video_present.epoch, WD_CLIENT_CONTENT_OWNER_VIDEO};
                        upload_has_ownership    = true;
                        content_texture_updated = true;
                    }
                    const struct wd_client_content_ownership_snapshot ownership =
                        wd_client_stream_ownership_snapshot(&state.stream_ownership);
                    const bool stale_video_needs_tile_restore      = video_result_stale && ownership.owner == WD_CLIENT_CONTENT_OWNER_TILES;
                    const bool allow_tile_upload                   = !video_result_stale || ownership.owner == WD_CLIENT_CONTENT_OWNER_TILES;

                    if (allow_tile_upload && !texture_needs_full_upload && !stale_video_needs_tile_restore)
                    {
                        std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
                        state.pending_dirty_tiles.take_rects(dirty_rects);
                        state.pending_dirty_rect_count.store(0, std::memory_order_release);
                        uploaded_ownership       = wd_client_stream_ownership_snapshot(&state.stream_ownership);
                        uploaded_ownership.epoch = state.pending_dirty_epoch;
                        upload_has_ownership     = true;
                    }

                    const uint64_t source_dirty_rect_count = dirty_rects.size();
                    if (remote_frame_dirty && !texture_needs_full_upload && !stale_video_needs_tile_restore && source_dirty_rect_count == 0)
                    {
                        state.stats.sdl_empty_remote_wakeups.fetch_add(1, std::memory_order_relaxed);
                        remote_frame_dirty = false;
                    }

                    DirtyTextureUploadPlan upload_plan{};
                    if (!texture_needs_full_upload && !stale_video_needs_tile_restore && source_dirty_rect_count != 0)
                    {
                        upload_plan = plan_dirty_texture_upload(dirty_rects, frame_width, frame_height, texture_upload_costs);
                    }

                    const bool cost_selected_full = !texture_needs_full_upload && !stale_video_needs_tile_restore &&
                                                    source_dirty_rect_count != 0 && upload_plan.mode == DirtyTextureUploadMode::Full;
                    const bool upload_full =
                        allow_tile_upload && (texture_needs_full_upload || stale_video_needs_tile_restore || cost_selected_full);
                    if (source_dirty_rect_count != 0)
                    {
                        state.stats.sdl_texture_source_dirty_rects.fetch_add(source_dirty_rect_count, std::memory_order_relaxed);
                        state.stats.sdl_texture_coalesced_dirty_rects.fetch_add(dirty_rects.size(), std::memory_order_relaxed);
                        state.stats.sdl_texture_source_pixels.fetch_add(upload_plan.source_pixels, std::memory_order_relaxed);
                        if (upload_plan.mode == DirtyTextureUploadMode::Bounds)
                        {
                            state.stats.sdl_texture_bounds_uploads.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (cost_selected_full)
                        {
                            state.stats.sdl_texture_cost_full_uploads.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    if (upload_full)
                    {
                        uploaded_ownership   = wd_client_stream_ownership_snapshot(&state.stream_ownership);
                        upload_has_ownership = true;
                    }

                    if (upload_full || source_dirty_rect_count != 0)
                    {
                        std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
                        if (upload_full)
                        {
                            claim_all_pending_tile_generations(state.pending_present_generation, state.presented_generation,
                                                               tile_generation_updates);
                        }
                        else
                        {
                            collect_base_tile_ids_for_rects(state.config, dirty_rects, dirty_tile_ids);
                            claim_pending_tile_generations(state.pending_present_generation, state.presented_generation, dirty_tile_ids,
                                                           tile_generation_updates);
                        }
                    }

                    bool tile_texture_updated = false;
                    if (upload_full)
                    {
                        {
                            std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
                            state.pending_dirty_tiles.clear();
                            state.pending_dirty_rect_count.store(0, std::memory_order_release);
                        }
                        tile_texture_updated =
                            upload_full_texture(state, texture, frame_width, frame_height, tile_upload_pixels, texture_upload_costs);
                    }
                    else if (allow_tile_upload && source_dirty_rect_count != 0)
                    {
                        tile_texture_updated =
                            upload_dirty_texture_rects(state, texture, dirty_rects, upload_plan, frame_width, frame_height,
                                                       tile_upload_pixels, staged_texture_rects, texture_upload_costs);
                    }

                    if ((upload_full || source_dirty_rect_count != 0) && !tile_texture_updated)
                    {
                        std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
                        requeue_tile_generation_updates(state.pending_present_generation, tile_generation_updates);
                        WD_LOG_ERROR("failed to update SDL tile texture: %s", SDL_GetError());
                        state.session.running.store(false, std::memory_order_relaxed);
                        break;
                    }
                    if (tile_texture_updated)
                    {
                        active_texture          = texture;
                        content_texture_updated = true;
                    }
                    remote_texture_updated = tile_texture_updated && (remote_frame_dirty || stale_video_needs_tile_restore);
                }
            }

            bool upload_stale = false;
            if (content_texture_updated && upload_has_ownership &&
                !wd_client_stream_ownership_is_current(&state.stream_ownership, uploaded_ownership.epoch, uploaded_ownership.owner))
            {
                upload_stale                                 = true;
                const struct wd_client_content_ownership_snapshot current = wd_client_stream_ownership_snapshot(&state.stream_ownership);
                if (!tile_generation_updates.empty() && current.owner == WD_CLIENT_CONTENT_OWNER_TILES)
                {
                    std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
                    requeue_tile_generation_updates(state.pending_present_generation, tile_generation_updates);
                }
                tile_generation_updates.clear();
                remote_texture_updated = false;
                video_present.valid    = false;
                if (current.owner == WD_CLIENT_CONTENT_OWNER_TILES)
                {
                    texture_needs_full_upload = true;
                    frame_dirty               = true;
                }
            }

            const bool should_present = !upload_stale && (local_frame_dirty || remote_texture_updated);
            if (should_present)
            {
                tile_telemetry_batch.clear();
                if (remote_texture_updated && !tile_generation_updates.empty())
                {
                    uint64_t content_epoch = 0;
                    {
                        std::lock_guard<std::mutex> content_lock(state.remote_content_mutex);
                        content_epoch = state.remote_content_epoch;
                    }
                    std::lock_guard<std::mutex> present_lock(state.present_mutex);
                    claim_tile_present_telemetry(state.pending_tile_telemetry, tile_generation_updates, content_epoch, wd_now_ns(),
                                                 tile_telemetry_batch);
                }
                if (!present_sdl_frame(state, renderer, active_texture, context_menu, remote_texture_updated, video_present,
                                       tile_telemetry_batch, last_present_ns))
                {
                    if (!tile_generation_updates.empty())
                    {
                        std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
                        requeue_tile_generation_updates(state.pending_present_generation, tile_generation_updates);
                    }
                    state.session.running.store(false, std::memory_order_relaxed);
                    break;
                }

                if (!tile_generation_updates.empty())
                {
                    {
                        std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
                        commit_tile_generation_updates(state.presented_generation, tile_generation_updates);
                    }
                    if (!tile_telemetry_batch.empty())
                    {
                        std::lock_guard<std::mutex> present_lock(state.present_mutex);
                        commit_tile_present_telemetry(state.pending_tile_telemetry, tile_generation_updates,
                                                      tile_telemetry_batch.content_epoch);
                    }
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

        const bool pending_remote_dirty = state.pending_dirty_rect_count.load(std::memory_order_acquire) != 0 ||
                                          state.pending_video_frame_dirty.load(std::memory_order_acquire);
        uint32_t   wait_ms              = 0;
        if (!frame_dirty && !pending_remote_dirty)
        {
            wait_ms = WD_CLIENT_FRAME_DELAY_MS;
        }
        else if (!frame_dirty)
        {
            wait_ms = video_hold_wait_ms != 0 ? video_hold_wait_ms : client_present_delay_ms(state, last_present_ns, wd_now_ns());
        }
        if (wait_ms != 0)
        {
            (void)state.render_wake.wait_for_change(render_wake_sequence, wait_ms);
        }
    }

    state.session.running.store(false, std::memory_order_relaxed);
    SDL_DestroyTexture(texture);
    SDL_DestroyTexture(video_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    free_cached_cursors();
    return 0;
}

} // namespace waydisplay
