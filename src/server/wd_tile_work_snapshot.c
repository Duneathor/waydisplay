#include "wd_tile_work_snapshot.h"

#include <string.h>

void wd_tile_work_snapshot_reset(struct wd_tile_work_snapshot* snapshot) {
    if (snapshot)
    {
        snapshot->count = 0;
    }
}

bool wd_tile_work_snapshot_add(struct wd_tile_work_snapshot* snapshot, uint16_t base_tile_id, bool selected, uint64_t dirty_epoch) {
    if (!snapshot || snapshot->count >= WD_WIRE_TILE_MAX_BASE_TILES)
    {
        return false;
    }

    for (uint16_t i = 0; i < snapshot->count; ++i)
    {
        if (snapshot->entries[i].base_tile_id == base_tile_id)
        {
            snapshot->entries[i].selected    = selected;
            snapshot->entries[i].dirty_epoch = dirty_epoch;
            return true;
        }
    }

    struct wd_tile_work_snapshot_entry* entry = &snapshot->entries[snapshot->count++];
    entry->base_tile_id                       = base_tile_id;
    entry->selected                           = selected;
    entry->dirty_epoch                        = dirty_epoch;
    return true;
}

bool wd_tile_work_snapshot_lookup(const struct wd_tile_work_snapshot* snapshot, uint16_t base_tile_id, bool* out_selected,
                                  uint64_t* out_dirty_epoch) {
    if (!snapshot)
    {
        return false;
    }

    for (uint16_t i = 0; i < snapshot->count; ++i)
    {
        const struct wd_tile_work_snapshot_entry* entry = &snapshot->entries[i];
        if (entry->base_tile_id != base_tile_id)
        {
            continue;
        }
        if (out_selected)
        {
            *out_selected = entry->selected;
        }
        if (out_dirty_epoch)
        {
            *out_dirty_epoch = entry->dirty_epoch;
        }
        return true;
    }
    return false;
}

bool wd_tile_work_snapshot_any_selected(const struct wd_tile_work_snapshot* snapshot, const uint16_t* base_tile_ids, uint16_t count) {
    if (!snapshot || !base_tile_ids)
    {
        return false;
    }

    for (uint16_t i = 0; i < count; ++i)
    {
        bool selected = false;
        if (wd_tile_work_snapshot_lookup(snapshot, base_tile_ids[i], &selected, NULL) && selected)
        {
            return true;
        }
    }
    return false;
}
