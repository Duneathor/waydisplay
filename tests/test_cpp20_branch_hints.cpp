#include <cstdint>

static_assert(__cplusplus >= 202002L, "WayDisplay C++ sources require C++20");

namespace {

constexpr uint32_t classify_packet(bool valid, bool complete) {
    if (!valid) [[unlikely]]
    {
        return 0;
    }

    if (complete) [[likely]]
    {
        return 2;
    }

    return 1;
}

} // namespace

int main() {
    static_assert(classify_packet(false, false) == 0);
    static_assert(classify_packet(true, false) == 1);
    static_assert(classify_packet(true, true) == 2);
    return 0;
}
