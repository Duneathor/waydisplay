#include "selection_sync.hpp"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_selection.h"
#include "wd_selection_delivery.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expr)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

constexpr uint8_t  SESSION_ID = 7;
constexpr uint64_t TOKEN      = 0x123456789abcdef0ull;

uint8_t* owned_text(const char* text, uint32_t* size) {
    *size      = static_cast<uint32_t>(std::strlen(text));
    auto* copy = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(*size) + 1u));
    CHECK(copy != nullptr);
    std::memcpy(copy, text, static_cast<size_t>(*size) + 1u);
    return copy;
}

std::vector<uint8_t> encode(const uint8_t* text, uint32_t size, uint8_t session_id = SESSION_ID, uint64_t token = TOKEN) {
    std::vector<uint8_t> payload(sizeof(wd_selection_payload_header) + size);
    uint32_t             payload_size = 0;
    CHECK(wd_selection_payload_encode(session_id, token, WD_SELECTION_MIME_TEXT_UTF8, text, size, payload.data(), payload.size(),
                                      &payload_size));
    payload.resize(payload_size);
    return payload;
}

wd_selection_text_view decode(const std::vector<uint8_t>& payload, uint8_t session_id = SESSION_ID, uint64_t token = TOKEN) {
    wd_selection_text_view view{};
    CHECK(wd_selection_payload_decode(payload.data(), static_cast<uint32_t>(payload.size()), session_id, token, &view));
    return view;
}

void test_server_to_client_and_echo_suppression() {
    wd_selection_delivery delivery;
    wd_selection_delivery_init(&delivery);

    uint32_t size        = 0;
    uint8_t* server_text = owned_text("from-server", &size);
    CHECK(wd_selection_delivery_set_owned(&delivery, server_text, size));

    const uint8_t* pending      = nullptr;
    uint32_t       pending_size = 0;
    CHECK(wd_selection_delivery_pending(&delivery, &pending, &pending_size));
    const auto payload = encode(pending, pending_size);
    wd_selection_delivery_mark_queued(&delivery);

    const wd_selection_text_view view = decode(payload);
    const std::string            received(reinterpret_cast<const char*>(view.data), view.size);

    waydisplay::ClientSelectionSyncState client;
    CHECK(waydisplay::client_selection_sync_should_apply(client, waydisplay::ClientSelectionKind::Clipboard, received));
    waydisplay::client_selection_sync_note_applied(client, waydisplay::ClientSelectionKind::Clipboard, received);

    CHECK(!waydisplay::client_selection_sync_should_send(client, waydisplay::ClientSelectionKind::Clipboard, received, false));
    CHECK(waydisplay::client_selection_sync_should_send(client, waydisplay::ClientSelectionKind::Clipboard, received, true));

    wd_selection_delivery_request(&delivery);
    CHECK(wd_selection_delivery_pending(&delivery, &pending, &pending_size));
    wd_selection_delivery_destroy(&delivery);
}

void test_client_to_server_and_reconnect() {
    waydisplay::ClientSelectionSyncState client;
    const std::string                    value = "from-client";
    CHECK(waydisplay::client_selection_sync_should_send(client, waydisplay::ClientSelectionKind::Clipboard, value, false));
    waydisplay::client_selection_sync_note_sent(client, waydisplay::ClientSelectionKind::Clipboard, value);
    CHECK(!waydisplay::client_selection_sync_should_send(client, waydisplay::ClientSelectionKind::Clipboard, value, false));

    const auto                   payload = encode(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
    const wd_selection_text_view view    = decode(payload);
    CHECK(view.size == value.size());
    CHECK(std::memcmp(view.data, value.data(), value.size()) == 0);

    wd_selection_text_view stale{};
    CHECK(!wd_selection_payload_decode(payload.data(), static_cast<uint32_t>(payload.size()), SESSION_ID, TOKEN + 1u, &stale));

    waydisplay::client_selection_sync_reset(client);
    CHECK(waydisplay::client_selection_sync_should_send(client, waydisplay::ClientSelectionKind::Clipboard, value, false));
}

void test_clear_round_trip() {
    wd_selection_delivery delivery;
    wd_selection_delivery_init(&delivery);
    CHECK(wd_selection_delivery_set_owned(&delivery, nullptr, 0));

    const uint8_t* pending      = reinterpret_cast<const uint8_t*>(0x1);
    uint32_t       pending_size = 99;
    CHECK(wd_selection_delivery_pending(&delivery, &pending, &pending_size));
    CHECK(pending == nullptr && pending_size == 0);

    const auto                   payload = encode(pending, pending_size);
    const wd_selection_text_view view    = decode(payload);
    CHECK(view.size == 0);

    wd_selection_delivery_destroy(&delivery);
}

} // namespace

int main() {
    test_server_to_client_and_echo_suppression();
    test_client_to_server_and_reconnect();
    test_clear_round_trip();
    return 0;
}
