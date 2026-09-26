#include "waydisplay/wd_buffer.h"

#include <stdatomic.h>
#include <stdlib.h>

struct wd_buffer {
    _Atomic uint32_t    refs;
    uint8_t*            data;
    size_t              size;
    size_t              capacity;
    wd_buffer_release_fn release;
    void*               release_data;
    bool                inline_storage;
    uint8_t              storage[];
};

struct wd_buffer* wd_buffer_alloc_padded(size_t size, size_t padding) {
    if (padding > SIZE_MAX - size)
    {
        return NULL;
    }
    const size_t capacity = size + padding;
    if (capacity > SIZE_MAX - sizeof(struct wd_buffer))
    {
        return NULL;
    }

    struct wd_buffer* buffer = calloc(1, sizeof(*buffer) + capacity);
    if (!buffer)
    {
        return NULL;
    }

    atomic_init(&buffer->refs, 1u);
    buffer->size           = size;
    buffer->capacity       = capacity;
    buffer->inline_storage = true;
    buffer->data           = capacity != 0 ? buffer->storage : NULL;
    return buffer;
}

struct wd_buffer* wd_buffer_alloc(size_t size) {
    return wd_buffer_alloc_padded(size, 0);
}

struct wd_buffer* wd_buffer_wrap(uint8_t* data, size_t size, wd_buffer_release_fn release, void* user_data) {
    if ((size != 0 && !data) || !release)
    {
        return NULL;
    }

    struct wd_buffer* buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
    {
        return NULL;
    }

    atomic_init(&buffer->refs, 1u);
    buffer->data         = data;
    buffer->size         = size;
    buffer->capacity     = size;
    buffer->release      = release;
    buffer->release_data = user_data;
    return buffer;
}

struct wd_buffer* wd_buffer_retain(struct wd_buffer* buffer) {
    if (!buffer)
    {
        return NULL;
    }

    uint32_t refs = atomic_load_explicit(&buffer->refs, memory_order_relaxed);
    for (;;)
    {
        if (refs == 0 || refs == UINT32_MAX)
        {
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(&buffer->refs, &refs, refs + 1u, memory_order_relaxed, memory_order_relaxed))
        {
            return buffer;
        }
    }
}

void wd_buffer_release(struct wd_buffer* buffer) {
    if (!buffer)
    {
        return;
    }

    if (atomic_fetch_sub_explicit(&buffer->refs, 1u, memory_order_acq_rel) != 1u)
    {
        return;
    }

    if (!buffer->inline_storage && buffer->release)
    {
        buffer->release(buffer->release_data, buffer->data, buffer->size);
    }
    free(buffer);
}

uint8_t* wd_buffer_data(struct wd_buffer* buffer) {
    return buffer ? buffer->data : NULL;
}

const uint8_t* wd_buffer_const_data(const struct wd_buffer* buffer) {
    return buffer ? buffer->data : NULL;
}

size_t wd_buffer_size(const struct wd_buffer* buffer) {
    return buffer ? buffer->size : 0;
}

size_t wd_buffer_capacity(const struct wd_buffer* buffer) {
    return buffer ? buffer->capacity : 0;
}

bool wd_buffer_range_valid(const struct wd_buffer* buffer, size_t offset, size_t size) {
    return buffer && offset <= buffer->size && size <= buffer->size - offset;
}
