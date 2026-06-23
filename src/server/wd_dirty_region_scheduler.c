#include "wd_dirty_region_scheduler.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct wd_dirty_region_heap_entry {
    uint64_t enqueued_ns;
    uint32_t serial;
    uint16_t region_id;
};

struct wd_dirty_region_scheduler {
    uint16_t                           capacity;
    uint16_t                           regions_x;
    uint16_t                           count;
    uint64_t                           starvation_ns;
    bool*                              queued;
    uint64_t*                          enqueued_ns;
    uint32_t*                          serial;
    uint64_t*                          queued_bits;
    size_t                             bit_word_count;
    struct wd_dirty_region_heap_entry* heap;
    size_t                             heap_count;
    size_t                             heap_capacity;
};

static bool heap_entry_less(const struct wd_dirty_region_heap_entry* lhs, const struct wd_dirty_region_heap_entry* rhs) {
    if (lhs->enqueued_ns != rhs->enqueued_ns)
    {
        return lhs->enqueued_ns < rhs->enqueued_ns;
    }
    return lhs->region_id < rhs->region_id;
}

static void heap_swap(struct wd_dirty_region_heap_entry* lhs, struct wd_dirty_region_heap_entry* rhs) {
    const struct wd_dirty_region_heap_entry tmp = *lhs;
    *lhs                                        = *rhs;
    *rhs                                        = tmp;
}

static void heap_sift_up(struct wd_dirty_region_scheduler* scheduler, size_t index) {
    while (index != 0)
    {
        const size_t parent = (index - 1u) / 2u;
        if (!heap_entry_less(&scheduler->heap[index], &scheduler->heap[parent]))
        {
            break;
        }
        heap_swap(&scheduler->heap[index], &scheduler->heap[parent]);
        index = parent;
    }
}

static void heap_sift_down(struct wd_dirty_region_scheduler* scheduler, size_t index) {
    for (;;)
    {
        const size_t left     = index * 2u + 1u;
        const size_t right    = left + 1u;
        size_t       smallest = index;
        if (left < scheduler->heap_count && heap_entry_less(&scheduler->heap[left], &scheduler->heap[smallest]))
        {
            smallest = left;
        }
        if (right < scheduler->heap_count && heap_entry_less(&scheduler->heap[right], &scheduler->heap[smallest]))
        {
            smallest = right;
        }
        if (smallest == index)
        {
            break;
        }
        heap_swap(&scheduler->heap[index], &scheduler->heap[smallest]);
        index = smallest;
    }
}

static bool heap_reserve(struct wd_dirty_region_scheduler* scheduler, size_t capacity) {
    if (scheduler->heap_capacity >= capacity)
    {
        return true;
    }
    size_t next_capacity = scheduler->heap_capacity ? scheduler->heap_capacity * 2u : 16u;
    if (next_capacity < capacity)
    {
        next_capacity = capacity;
    }
    struct wd_dirty_region_heap_entry* next = realloc(scheduler->heap, next_capacity * sizeof(*next));
    if (!next)
    {
        return false;
    }
    scheduler->heap          = next;
    scheduler->heap_capacity = next_capacity;
    return true;
}

static bool heap_push(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id) {
    if (!heap_reserve(scheduler, scheduler->heap_count + 1u))
    {
        return false;
    }
    struct wd_dirty_region_heap_entry* entry = &scheduler->heap[scheduler->heap_count];
    entry->enqueued_ns                       = scheduler->enqueued_ns[region_id];
    entry->serial                            = scheduler->serial[region_id];
    entry->region_id                         = region_id;
    heap_sift_up(scheduler, scheduler->heap_count);
    scheduler->heap_count++;
    return true;
}

static void heap_pop(struct wd_dirty_region_scheduler* scheduler) {
    if (scheduler->heap_count == 0)
    {
        return;
    }
    scheduler->heap_count--;
    if (scheduler->heap_count != 0)
    {
        scheduler->heap[0] = scheduler->heap[scheduler->heap_count];
        heap_sift_down(scheduler, 0);
    }
}

static bool heap_entry_current(const struct wd_dirty_region_scheduler* scheduler, const struct wd_dirty_region_heap_entry* entry) {
    return entry->region_id < scheduler->capacity && scheduler->queued[entry->region_id] &&
           scheduler->serial[entry->region_id] == entry->serial && scheduler->enqueued_ns[entry->region_id] == entry->enqueued_ns;
}

static void heap_prune(struct wd_dirty_region_scheduler* scheduler) {
    while (scheduler->heap_count != 0 && !heap_entry_current(scheduler, &scheduler->heap[0]))
    {
        heap_pop(scheduler);
    }
}

static void bit_set(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id) {
    scheduler->queued_bits[region_id / 64u] |= UINT64_C(1) << (region_id % 64u);
}

static void bit_clear(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id) {
    scheduler->queued_bits[region_id / 64u] &= ~(UINT64_C(1) << (region_id % 64u));
}

static uint16_t find_next_queued(const struct wd_dirty_region_scheduler* scheduler, uint16_t cursor_region_id) {
    if (!scheduler || scheduler->count == 0)
    {
        return UINT16_MAX;
    }

    const uint32_t start = ((uint32_t)cursor_region_id + 1u) % scheduler->capacity;
    for (uint32_t pass = 0; pass < 2u; ++pass)
    {
        const uint32_t begin = pass == 0 ? start : 0u;
        const uint32_t end   = pass == 0 ? scheduler->capacity : start;
        if (begin >= end)
        {
            continue;
        }

        size_t       word      = begin / 64u;
        const size_t last_word = (end - 1u) / 64u;
        uint64_t     bits      = scheduler->queued_bits[word] & (~UINT64_C(0) << (begin % 64u));
        while (word <= last_word)
        {
            if (word == last_word && end % 64u != 0)
            {
                bits &= (UINT64_C(1) << (end % 64u)) - 1u;
            }
            if (bits != 0)
            {
                const uint32_t bit = (uint32_t)__builtin_ctzll(bits);
                const uint32_t id  = (uint32_t)word * 64u + bit;
                if (id < scheduler->capacity)
                {
                    return (uint16_t)id;
                }
            }
            word++;
            if (word > last_word)
            {
                break;
            }
            bits = scheduler->queued_bits[word];
        }
    }
    return UINT16_MAX;
}

struct wd_dirty_region_scheduler* wd_dirty_region_scheduler_create(uint16_t capacity, uint16_t regions_x, uint64_t starvation_ns) {
    if (capacity == 0 || regions_x == 0)
    {
        return NULL;
    }
    struct wd_dirty_region_scheduler* scheduler = calloc(1, sizeof(*scheduler));
    if (!scheduler)
    {
        return NULL;
    }
    scheduler->capacity       = capacity;
    scheduler->regions_x      = regions_x;
    scheduler->starvation_ns  = starvation_ns;
    scheduler->bit_word_count = ((size_t)capacity + 63u) / 64u;
    scheduler->queued         = calloc(capacity, sizeof(*scheduler->queued));
    scheduler->enqueued_ns    = calloc(capacity, sizeof(*scheduler->enqueued_ns));
    scheduler->serial         = calloc(capacity, sizeof(*scheduler->serial));
    scheduler->queued_bits    = calloc(scheduler->bit_word_count, sizeof(*scheduler->queued_bits));
    if (!scheduler->queued || !scheduler->enqueued_ns || !scheduler->serial || !scheduler->queued_bits)
    {
        wd_dirty_region_scheduler_destroy(scheduler);
        return NULL;
    }
    return scheduler;
}

void wd_dirty_region_scheduler_destroy(struct wd_dirty_region_scheduler* scheduler) {
    if (!scheduler)
    {
        return;
    }
    free(scheduler->queued);
    free(scheduler->enqueued_ns);
    free(scheduler->serial);
    free(scheduler->queued_bits);
    free(scheduler->heap);
    free(scheduler);
}

void wd_dirty_region_scheduler_reset(struct wd_dirty_region_scheduler* scheduler) {
    if (!scheduler)
    {
        return;
    }
    memset(scheduler->queued, 0, scheduler->capacity * sizeof(*scheduler->queued));
    memset(scheduler->enqueued_ns, 0, scheduler->capacity * sizeof(*scheduler->enqueued_ns));
    memset(scheduler->queued_bits, 0, scheduler->bit_word_count * sizeof(*scheduler->queued_bits));
    scheduler->count      = 0;
    scheduler->heap_count = 0;
}

bool wd_dirty_region_scheduler_enqueue(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id, uint64_t now_ns) {
    if (!scheduler || region_id >= scheduler->capacity)
    {
        return false;
    }
    if (scheduler->queued[region_id])
    {
        return true;
    }
    if (scheduler->enqueued_ns[region_id] == 0)
    {
        scheduler->enqueued_ns[region_id] = now_ns != 0 ? now_ns : 1u;
    }
    scheduler->serial[region_id]++;
    if (scheduler->serial[region_id] == 0)
    {
        scheduler->serial[region_id] = 1;
    }
    scheduler->queued[region_id] = true;
    bit_set(scheduler, region_id);
    scheduler->count++;
    if (!heap_push(scheduler, region_id))
    {
        scheduler->count--;
        scheduler->queued[region_id] = false;
        bit_clear(scheduler, region_id);
        return false;
    }
    return true;
}

bool wd_dirty_region_scheduler_take(struct wd_dirty_region_scheduler* scheduler, uint16_t cursor_region_id, uint64_t now_ns,
                                    uint16_t* out_region_id) {
    if (!scheduler || !out_region_id || scheduler->count == 0)
    {
        return false;
    }

    heap_prune(scheduler);
    uint16_t selected = UINT16_MAX;
    if (scheduler->heap_count != 0 && scheduler->starvation_ns != 0)
    {
        const struct wd_dirty_region_heap_entry* oldest = &scheduler->heap[0];
        if (now_ns >= oldest->enqueued_ns && now_ns - oldest->enqueued_ns >= scheduler->starvation_ns)
        {
            selected = oldest->region_id;
        }
    }
    if (selected == UINT16_MAX)
    {
        selected = find_next_queued(scheduler, cursor_region_id);
    }
    if (selected == UINT16_MAX || !scheduler->queued[selected])
    {
        return false;
    }

    scheduler->queued[selected] = false;
    bit_clear(scheduler, selected);
    scheduler->count--;
    *out_region_id = selected;
    return true;
}

void wd_dirty_region_scheduler_forget(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id) {
    if (!scheduler || region_id >= scheduler->capacity)
    {
        return;
    }
    if (scheduler->queued[region_id])
    {
        scheduler->queued[region_id] = false;
        bit_clear(scheduler, region_id);
        if (scheduler->count != 0)
        {
            scheduler->count--;
        }
    }
    scheduler->enqueued_ns[region_id] = 0;
    scheduler->serial[region_id]++;
}

bool wd_dirty_region_scheduler_contains(const struct wd_dirty_region_scheduler* scheduler, uint16_t region_id) {
    return scheduler && region_id < scheduler->capacity && scheduler->queued[region_id];
}

uint16_t wd_dirty_region_scheduler_count(const struct wd_dirty_region_scheduler* scheduler) {
    return scheduler ? scheduler->count : 0;
}

uint64_t wd_dirty_region_scheduler_enqueued_ns(const struct wd_dirty_region_scheduler* scheduler, uint16_t region_id) {
    return scheduler && region_id < scheduler->capacity ? scheduler->enqueued_ns[region_id] : 0;
}
