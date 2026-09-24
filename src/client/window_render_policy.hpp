#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstdint>

namespace waydisplay {

/* Keep event-driven focus separate from minimized/hidden visibility. An
 * unfocused window can continue presenting normally. */
enum class ClientWindowFocusChange { None, Lost, Gained };

inline bool client_window_focus_after_event(bool focused, ClientWindowFocusChange change) {
    switch (change)
    {
    case ClientWindowFocusChange::Lost: return false;
    case ClientWindowFocusChange::Gained: return true;
    case ClientWindowFocusChange::None: return focused;
    }
    return focused;
}

inline uint32_t client_window_feedback_flags(bool visible, bool focused) {
    return (visible ? static_cast<uint32_t>(WD_CLIENT_STATS_RENDER_VISIBLE) : 0u) |
           (visible && focused ? static_cast<uint32_t>(WD_CLIENT_STATS_WINDOW_FOCUSED) : 0u);
}

} // namespace waydisplay
