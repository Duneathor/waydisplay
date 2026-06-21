#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_dirty_region_scheduler;

struct wd_dirty_region_scheduler* wd_dirty_region_scheduler_create(uint16_t capacity, uint16_t regions_x,
                                                                    uint64_t starvation_ns);
void wd_dirty_region_scheduler_destroy(struct wd_dirty_region_scheduler* scheduler);
void wd_dirty_region_scheduler_reset(struct wd_dirty_region_scheduler* scheduler);

bool wd_dirty_region_scheduler_enqueue(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id,
                                       uint64_t now_ns);
bool wd_dirty_region_scheduler_take(struct wd_dirty_region_scheduler* scheduler, uint16_t cursor_region_id,
                                    uint64_t now_ns, uint16_t* out_region_id);
void wd_dirty_region_scheduler_forget(struct wd_dirty_region_scheduler* scheduler, uint16_t region_id);

bool wd_dirty_region_scheduler_contains(const struct wd_dirty_region_scheduler* scheduler, uint16_t region_id);
uint16_t wd_dirty_region_scheduler_count(const struct wd_dirty_region_scheduler* scheduler);
uint64_t wd_dirty_region_scheduler_enqueued_ns(const struct wd_dirty_region_scheduler* scheduler,
                                               uint16_t region_id);

#ifdef __cplusplus
}
#endif
