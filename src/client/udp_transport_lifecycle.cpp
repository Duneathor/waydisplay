#include "udp_transport_lifecycle.hpp"

namespace waydisplay {

ClientUdpFallbackAction client_udp_fallback_action(ClientAsyncUdpDetachResult detach_result, bool socket_is_open) {
    if (!socket_is_open)
    {
        return ClientUdpFallbackAction::Abort;
    }
    return detach_result == ClientAsyncUdpDetachResult::Detached ? ClientUdpFallbackAction::ReuseSocket
                                                                 : ClientUdpFallbackAction::ReplaceSocket;
}

} // namespace waydisplay
