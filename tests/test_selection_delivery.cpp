#include "wd_selection_delivery.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(expr)                                                                                 \
    do                                                                                              \
    {                                                                                               \
        if (!(expr))                                                                                \
        {                                                                                           \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);       \
            std::exit(1);                                                                           \
        }                                                                                           \
    } while (0)

static uint8_t* copy_text(const char* text, uint32_t* size) {
    *size = static_cast<uint32_t>(std::strlen(text));
    auto* copy = static_cast<uint8_t*>(std::malloc(*size + 1u));
    CHECK(copy != nullptr);
    std::memcpy(copy, text, *size + 1u);
    return copy;
}

int main() {
    wd_selection_delivery delivery;
    wd_selection_delivery_init(&delivery);

    const uint8_t* text = nullptr;
    uint32_t size = 0;
    CHECK(!wd_selection_delivery_pending(&delivery, &text, &size));

    uint32_t first_size = 0;
    uint8_t* first = copy_text("first", &first_size);
    CHECK(wd_selection_delivery_set_owned(&delivery, first, first_size));
    CHECK(wd_selection_delivery_pending(&delivery, &text, &size));
    CHECK(size == 5 && std::memcmp(text, "first", 5) == 0);

    wd_selection_delivery_mark_queued(&delivery);
    CHECK(!wd_selection_delivery_pending(&delivery, &text, &size));
    wd_selection_delivery_request(&delivery);
    CHECK(wd_selection_delivery_pending(&delivery, &text, &size));

    uint32_t latest_size = 0;
    uint8_t* latest = copy_text("latest", &latest_size);
    CHECK(wd_selection_delivery_set_owned(&delivery, latest, latest_size));
    CHECK(wd_selection_delivery_pending(&delivery, &text, &size));
    CHECK(size == 6 && std::memcmp(text, "latest", 6) == 0);

    wd_selection_delivery_mark_unknown(&delivery);
    wd_selection_delivery_request(&delivery);
    CHECK(!wd_selection_delivery_pending(&delivery, &text, &size));

    CHECK(wd_selection_delivery_set_owned(&delivery, nullptr, 0));
    CHECK(wd_selection_delivery_pending(&delivery, &text, &size));
    CHECK(text == nullptr && size == 0);

    wd_selection_delivery_destroy(&delivery);
    return 0;
}
