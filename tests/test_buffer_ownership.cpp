#include "waydisplay/wd_buffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                     \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

struct ReleaseProbe {
    std::atomic<unsigned> calls{0};
    uint8_t*              expected_data = nullptr;
    size_t                expected_size = 0;
};

void release_external(void* user_data, uint8_t* data, size_t size) {
    auto* probe = static_cast<ReleaseProbe*>(user_data);
    CHECK(probe != nullptr);
    CHECK(data == probe->expected_data);
    CHECK(size == probe->expected_size);
    probe->calls.fetch_add(1, std::memory_order_relaxed);
    std::free(data);
}

void release_zero(void* user_data, uint8_t* data, size_t size) {
    auto* calls = static_cast<std::atomic<unsigned>*>(user_data);
    CHECK(data == nullptr);
    CHECK(size == 0);
    calls->fetch_add(1, std::memory_order_relaxed);
}

void test_inline_storage_and_ranges() {
    wd_buffer* buffer = wd_buffer_alloc(257);
    CHECK(buffer != nullptr);
    CHECK(wd_buffer_size(buffer) == 257);
    CHECK(wd_buffer_data(buffer) != nullptr);
    CHECK(wd_buffer_const_data(buffer) == wd_buffer_data(buffer));

    for (size_t i = 0; i < wd_buffer_size(buffer); ++i)
    {
        wd_buffer_data(buffer)[i] = static_cast<uint8_t>((i * 17u) & 0xffu);
    }

    wd_buffer* second = wd_buffer_retain(buffer);
    CHECK(second == buffer);
    CHECK(wd_buffer_data(second)[113] == static_cast<uint8_t>((113u * 17u) & 0xffu));

    wd_buffer_data(second)[113] = 0x5a;
    CHECK(wd_buffer_data(buffer)[113] == 0x5a);

    CHECK(wd_buffer_range_valid(buffer, 0, 257));
    CHECK(wd_buffer_range_valid(buffer, 257, 0));
    CHECK(wd_buffer_range_valid(buffer, 17, 0));
    CHECK(!wd_buffer_range_valid(buffer, 258, 0));
    CHECK(!wd_buffer_range_valid(buffer, 256, 2));
    CHECK(!wd_buffer_range_valid(nullptr, 0, 0));

    wd_buffer_release(second);
    CHECK(wd_buffer_data(buffer)[113] == 0x5a);
    wd_buffer_release(buffer);
}

void test_external_owner_released_once() {
    auto* bytes = static_cast<uint8_t*>(std::malloc(4096));
    CHECK(bytes != nullptr);
    std::memset(bytes, 0xa5, 4096);

    ReleaseProbe probe{};
    probe.expected_data = bytes;
    probe.expected_size = 4096;

    wd_buffer* buffer = wd_buffer_wrap(bytes, 4096, release_external, &probe);
    CHECK(buffer != nullptr);
    CHECK(wd_buffer_size(buffer) == 4096);
    CHECK(wd_buffer_data(buffer) == bytes);

    constexpr unsigned ThreadCount = 8;
    constexpr unsigned Iterations  = 25000;
    std::vector<std::thread> threads;
    threads.reserve(ThreadCount);
    for (unsigned thread = 0; thread < ThreadCount; ++thread)
    {
        threads.emplace_back([buffer]() {
            for (unsigned i = 0; i < Iterations; ++i)
            {
                wd_buffer* retained = wd_buffer_retain(buffer);
                CHECK(retained == buffer);
                const size_t offset = static_cast<size_t>(i) & 4095u;
                CHECK(wd_buffer_data(retained)[offset] == 0xa5);
                wd_buffer_release(retained);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    CHECK(probe.calls.load(std::memory_order_relaxed) == 0);
    wd_buffer_release(buffer);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);
}

void test_padded_storage() {
    constexpr size_t Logical = 113;
    constexpr size_t Padding = 64;
    wd_buffer* buffer = wd_buffer_alloc_padded(Logical, Padding);
    CHECK(buffer != nullptr);
    CHECK(wd_buffer_size(buffer) == Logical);
    CHECK(wd_buffer_capacity(buffer) == Logical + Padding);
    CHECK(wd_buffer_data(buffer) != nullptr);
    for (size_t i = 0; i < Logical; ++i)
    {
        wd_buffer_data(buffer)[i] = static_cast<uint8_t>(i | 1u);
    }
    for (size_t i = Logical; i < Logical + Padding; ++i)
    {
        CHECK(wd_buffer_data(buffer)[i] == 0);
    }
    CHECK(wd_buffer_alloc_padded(SIZE_MAX, 1) == nullptr);
    CHECK(wd_buffer_alloc_padded(SIZE_MAX - 3u, 8u) == nullptr);
    wd_buffer_release(buffer);
}

void test_zero_length_and_invalid_wraps() {
    wd_buffer* empty = wd_buffer_alloc(0);
    CHECK(empty != nullptr);
    CHECK(wd_buffer_size(empty) == 0);
    CHECK(wd_buffer_data(empty) == nullptr);
    CHECK(wd_buffer_range_valid(empty, 0, 0));
    wd_buffer_release(empty);

    std::atomic<unsigned> calls{0};
    wd_buffer* wrapped_empty = wd_buffer_wrap(nullptr, 0, release_zero, &calls);
    CHECK(wrapped_empty != nullptr);
    wd_buffer_release(wrapped_empty);
    CHECK(calls.load(std::memory_order_relaxed) == 1);

    uint8_t byte = 0;
    CHECK(wd_buffer_wrap(nullptr, 1, release_zero, &calls) == nullptr);
    CHECK(wd_buffer_wrap(&byte, 1, nullptr, nullptr) == nullptr);
    CHECK(wd_buffer_retain(nullptr) == nullptr);
    wd_buffer_release(nullptr);
}

} // namespace

int main() {
    test_inline_storage_and_ranges();
    test_external_owner_released_once();
    test_padded_storage();
    test_zero_length_and_invalid_wraps();
    return 0;
}
