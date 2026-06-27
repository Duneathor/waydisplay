#include "client_state.hpp"
#include "content_order.hpp"

#include <cstdlib>
#include <iostream>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    ClientState state;

    client_reset_content_epoch(state, 4, WD_CLIENT_CONTENT_OWNER_TILES);
    require(state.remote_content_epoch == 4, "reset should publish the initial content epoch");
    require(state.remote_content_owner == WD_CLIENT_CONTENT_OWNER_TILES, "reset should publish tile ownership");

    require(client_accept_content_epoch(state, 5, WD_CLIENT_CONTENT_OWNER_VIDEO) == ClientContentEpochDecision::Advanced,
            "a newer video epoch should advance ownership");
    require(state.remote_content_epoch == 5, "advanced ownership should retain the newer epoch");
    require(state.remote_content_owner == WD_CLIENT_CONTENT_OWNER_VIDEO, "advanced ownership should retain video ownership");

    require(client_accept_content_epoch(state, 4, WD_CLIENT_CONTENT_OWNER_TILES) == ClientContentEpochDecision::Stale,
            "an older tile epoch should remain stale");
    return 0;
}
