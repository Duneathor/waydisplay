#include "waydisplay/wd_protocol.h"
#include "wd_selection_capture.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static void test_chunked_capture() {
    wd_selection_capture_buffer buffer;
    wd_selection_capture_buffer_init(&buffer);

    const uint8_t first[]  = {'h', 'e', 'l'};
    const uint8_t second[] = {'l', 'o', 0, 0};
    CHECK(wd_selection_capture_buffer_append(&buffer, first, sizeof(first)));
    CHECK(wd_selection_capture_buffer_append(&buffer, second, sizeof(second)));

    uint8_t* text = nullptr;
    uint32_t size = 0;
    CHECK(wd_selection_capture_buffer_finish(&buffer, &text, &size));
    CHECK(size == 5);
    CHECK(std::memcmp(text, "hello", 5) == 0);
    CHECK(text[5] == 0);

    std::free(text);
    wd_selection_capture_buffer_destroy(&buffer);
}

static void test_empty_capture() {
    wd_selection_capture_buffer buffer;
    wd_selection_capture_buffer_init(&buffer);

    uint8_t* text = nullptr;
    uint32_t size = 99;
    CHECK(wd_selection_capture_buffer_finish(&buffer, &text, &size));
    CHECK(text != nullptr);
    CHECK(size == 0);
    CHECK(text[0] == 0);

    std::free(text);
    wd_selection_capture_buffer_destroy(&buffer);
}

static void test_invalid_text_rejected() {
    wd_selection_capture_buffer buffer;
    wd_selection_capture_buffer_init(&buffer);

    const uint8_t embedded_nul[] = {'a', 0, 'b'};
    CHECK(wd_selection_capture_buffer_append(&buffer, embedded_nul, sizeof(embedded_nul)));

    uint8_t* text = nullptr;
    uint32_t size = 0;
    CHECK(!wd_selection_capture_buffer_finish(&buffer, &text, &size));
    CHECK(text == nullptr);
    wd_selection_capture_buffer_destroy(&buffer);

    wd_selection_capture_buffer_init(&buffer);
    const uint8_t invalid_utf8[] = {0xc0, 0xaf};
    CHECK(wd_selection_capture_buffer_append(&buffer, invalid_utf8, sizeof(invalid_utf8)));
    CHECK(!wd_selection_capture_buffer_finish(&buffer, &text, &size));
    wd_selection_capture_buffer_destroy(&buffer);
}

static void test_maximum_with_terminator() {
    wd_selection_capture_buffer buffer;
    wd_selection_capture_buffer_init(&buffer);

    std::vector<uint8_t> maximum(WD_SELECTION_MAX_TEXT_BYTES, 'x');
    CHECK(wd_selection_capture_buffer_append(&buffer, maximum.data(), maximum.size()));
    const uint8_t terminator = 0;
    CHECK(wd_selection_capture_buffer_append(&buffer, &terminator, 1));

    uint8_t* text = nullptr;
    uint32_t size = 0;
    CHECK(wd_selection_capture_buffer_finish(&buffer, &text, &size));
    CHECK(size == WD_SELECTION_MAX_TEXT_BYTES);
    std::free(text);
    wd_selection_capture_buffer_destroy(&buffer);
}

static void test_oversize_rejected() {
    wd_selection_capture_buffer buffer;
    wd_selection_capture_buffer_init(&buffer);

    std::vector<uint8_t> maximum(WD_SELECTION_MAX_TEXT_BYTES, 'x');
    CHECK(wd_selection_capture_buffer_append(&buffer, maximum.data(), maximum.size()));

    const uint8_t extras[] = {0, 'y'};
    CHECK(!wd_selection_capture_buffer_append(&buffer, extras, sizeof(extras)));

    uint8_t* text = nullptr;
    uint32_t size = 0;
    CHECK(!wd_selection_capture_buffer_finish(&buffer, &text, &size));
    wd_selection_capture_buffer_destroy(&buffer);
}

int main() {
    test_chunked_capture();
    test_empty_capture();
    test_invalid_text_rejected();
    test_maximum_with_terminator();
    test_oversize_rejected();
    return 0;
}
