#include "wd_dirty_region_scheduler.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    wd_dirty_region_scheduler* scheduler = wd_dirty_region_scheduler_create(128, 16, 100);
    assert(scheduler);

    for (uint16_t id = 0; id < 128; ++id)
    {
        assert(wd_dirty_region_scheduler_enqueue(scheduler, id, 1000 + id));
    }
    assert(wd_dirty_region_scheduler_count(scheduler) == 128);

    std::vector<bool> seen(128, false);
    uint16_t cursor = 0;
    for (uint16_t i = 0; i < 128; ++i)
    {
        uint16_t id = 0;
        assert(wd_dirty_region_scheduler_take(scheduler, cursor, 1050, &id));
        assert(id < 128 && !seen[id]);
        seen[id] = true;
        cursor = id;
    }
    assert(wd_dirty_region_scheduler_count(scheduler) == 0);
    wd_dirty_region_scheduler_reset(scheduler);

    assert(wd_dirty_region_scheduler_enqueue(scheduler, 90, 100));
    assert(wd_dirty_region_scheduler_enqueue(scheduler, 2, 250));
    uint16_t id = 0;
    assert(wd_dirty_region_scheduler_take(scheduler, 1, 250, &id));
    assert(id == 90); // oldest entry crossed the starvation bound
    assert(wd_dirty_region_scheduler_enqueued_ns(scheduler, 90) == 100);
    assert(wd_dirty_region_scheduler_enqueue(scheduler, 90, 999)); // requeue preserves original age
    assert(wd_dirty_region_scheduler_enqueued_ns(scheduler, 90) == 100);
    wd_dirty_region_scheduler_forget(scheduler, 90);
    assert(wd_dirty_region_scheduler_enqueued_ns(scheduler, 90) == 0);

    wd_dirty_region_scheduler_reset(scheduler);
    assert(wd_dirty_region_scheduler_count(scheduler) == 0);
    wd_dirty_region_scheduler_destroy(scheduler);
    return 0;
}
