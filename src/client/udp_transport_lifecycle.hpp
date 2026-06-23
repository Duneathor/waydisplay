#pragma once

#include <cstdint>

namespace waydisplay {

enum class ClientAsyncUdpDetachResult : uint8_t {
    Detached = 0,
    SocketStillOwned,
};

enum class ClientUdpFallbackAction : uint8_t {
    ReuseSocket = 0,
    ReplaceSocket,
    Abort,
};

ClientUdpFallbackAction client_udp_fallback_action(ClientAsyncUdpDetachResult detach_result, bool socket_is_open);

} // namespace waydisplay
