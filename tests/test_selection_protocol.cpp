#include "waydisplay/wd_selection.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);   \
            failures++;                                                                             \
        }                                                                                           \
    } while (false)

void test_text_validation() {
    const uint8_t ascii[] = {'h', 'e', 'l', 'l', 'o'};
    const uint8_t utf8[]  = {0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80};
    const uint8_t nul[]   = {'a', 0, 'b'};
    const uint8_t overlong[] = {0xc0, 0xaf};
    const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
    const uint8_t too_high[] = {0xf4, 0x90, 0x80, 0x80};
    const uint8_t truncated[] = {0xe2, 0x82};

    CHECK(wd_selection_text_is_valid(nullptr, 0));
    CHECK(wd_selection_text_is_valid(ascii, sizeof(ascii)));
    CHECK(wd_selection_text_is_valid(utf8, sizeof(utf8)));
    CHECK(!wd_selection_text_is_valid(nullptr, 1));
    CHECK(!wd_selection_text_is_valid(nul, sizeof(nul)));
    CHECK(!wd_selection_text_is_valid(overlong, sizeof(overlong)));
    CHECK(!wd_selection_text_is_valid(surrogate, sizeof(surrogate)));
    CHECK(!wd_selection_text_is_valid(too_high, sizeof(too_high)));
    CHECK(!wd_selection_text_is_valid(truncated, sizeof(truncated)));
}

void test_normalization() {
    const uint8_t terminated[] = {'o', 'k', 0, 0};
    const uint8_t embedded[]   = {'o', 0, 'k', 0};
    uint32_t normalized = 99;

    CHECK(wd_selection_text_normalize_size(terminated, sizeof(terminated), &normalized));
    CHECK(normalized == 2);
    CHECK(!wd_selection_text_normalize_size(embedded, sizeof(embedded), &normalized));
    CHECK(!wd_selection_text_normalize_size(nullptr, 1, &normalized));
    CHECK(!wd_selection_text_normalize_size(nullptr, 0, nullptr));
}

void test_payload_roundtrip() {
    constexpr uint8_t session = 7;
    constexpr uint64_t token = 0x1020304050607080ull;
    const uint8_t text[] = {'c', 'l', 'i', 'p', ' ', 0xe2, 0x82, 0xac};

    std::vector<uint8_t> payload(sizeof(wd_selection_payload_header) + sizeof(text));
    uint32_t payload_size = 0;
    CHECK(wd_selection_payload_encode(session, token, WD_SELECTION_MIME_TEXT_UTF8,
                                      text, sizeof(text), payload.data(), payload.size(),
                                      &payload_size));
    CHECK(payload_size == payload.size());

    wd_selection_text_view view{};
    CHECK(wd_selection_payload_decode(payload.data(), payload_size, session, token, &view));
    CHECK(view.mime_type == WD_SELECTION_MIME_TEXT_UTF8);
    CHECK(view.size == sizeof(text));
    CHECK(std::memcmp(view.data, text, sizeof(text)) == 0);

    CHECK(!wd_selection_payload_decode(payload.data(), payload_size, session + 1, token, &view));
    CHECK(!wd_selection_payload_decode(payload.data(), payload_size, session, token + 1, &view));
    CHECK(!wd_selection_payload_decode(payload.data(), payload_size - 1, session, token, &view));

    auto* header = reinterpret_cast<wd_selection_payload_header*>(payload.data());
    const uint16_t saved_mime = header->mime_type;
    header->mime_type = 99;
    CHECK(!wd_selection_payload_decode(payload.data(), payload_size, session, token, &view));
    header->mime_type = saved_mime;

    payload.back() = 0;
    CHECK(!wd_selection_payload_decode(payload.data(), payload_size, session, token, &view));
}

void test_encode_rejections() {
    uint8_t payload[64]{};
    uint32_t size = 123;
    const uint8_t embedded_nul[] = {'a', 0, 'b'};

    CHECK(!wd_selection_payload_encode(0, 1, WD_SELECTION_MIME_TEXT_UTF8,
                                       nullptr, 0, payload, sizeof(payload), &size));
    CHECK(size == 0);
    CHECK(!wd_selection_payload_encode(1, 0, WD_SELECTION_MIME_TEXT_UTF8,
                                       nullptr, 0, payload, sizeof(payload), &size));
    CHECK(!wd_selection_payload_encode(1, 1, 99, nullptr, 0,
                                       payload, sizeof(payload), &size));
    CHECK(!wd_selection_payload_encode(1, 1, WD_SELECTION_MIME_TEXT_UTF8,
                                       embedded_nul, sizeof(embedded_nul),
                                       payload, sizeof(payload), &size));
    CHECK(!wd_selection_payload_encode(1, 1, WD_SELECTION_MIME_TEXT_UTF8,
                                       nullptr, 0, payload,
                                       sizeof(wd_selection_payload_header) - 1, &size));
    CHECK(wd_selection_payload_encode(1, 1, WD_SELECTION_MIME_TEXT_UTF8,
                                      nullptr, 0, payload, sizeof(payload), &size));
    CHECK(size == sizeof(wd_selection_payload_header));
}

} // namespace

int main() {
    test_text_validation();
    test_normalization();
    test_payload_roundtrip();
    test_encode_rejections();
    return failures == 0 ? 0 : 1;
}
