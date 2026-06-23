#include "wd_dirty_region_scheduler.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    wd_dirty_region_scheduler* scheduler = wd_dirty_region_scheduler_create(128, 16, 100);
    require(scheduler != nullptr, "scheduler creation should succeed");

    for (uint16_t id = 0; id < 128; ++id)
    {
        require(wd_dirty_region_scheduler_enqueue(scheduler, id, 1000 + id), "every region should enqueue once");
    }
    require(wd_dirty_region_scheduler_count(scheduler) == 128, "scheduler should contain every enqueued region");

    std::vector<bool> seen(128, false);
    uint16_t          cursor = 0;
    for (uint16_t i = 0; i < 128; ++i)
    {
        uint16_t id = 0;
        require(wd_dirty_region_scheduler_take(scheduler, cursor, 1050, &id), "scheduler should return every queued region");
        require(id < 128 && !seen[id], "scheduler should not return a duplicate region");
        seen[id] = true;
        cursor   = id;
    }
    require(wd_dirty_region_scheduler_count(scheduler) == 0, "scheduler should be empty after taking every region");
    wd_dirty_region_scheduler_reset(scheduler);

    require(wd_dirty_region_scheduler_enqueue(scheduler, 90, 100), "old region should enqueue");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 2, 250), "new region should enqueue");
    uint16_t id = 0;
    require(wd_dirty_region_scheduler_take(scheduler, 1, 250, &id), "scheduler should return a starvation candidate");
    require(id == 90, "oldest entry should cross the starvation bound");
    require(wd_dirty_region_scheduler_enqueued_ns(scheduler, 90) == 100, "scheduler should retain the original enqueue time");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 90, 999), "a taken region should be requeueable");
    require(wd_dirty_region_scheduler_enqueued_ns(scheduler, 90) == 100, "requeue should preserve the original age");
    wd_dirty_region_scheduler_forget(scheduler, 90);
    require(wd_dirty_region_scheduler_enqueued_ns(scheduler, 90) == 0, "forget should clear the enqueue time");

    wd_dirty_region_scheduler_reset(scheduler);
    require(wd_dirty_region_scheduler_count(scheduler) == 0, "reset should clear all scheduled regions");
    wd_dirty_region_scheduler_destroy(scheduler);
    return EXIT_SUCCESS;
}
