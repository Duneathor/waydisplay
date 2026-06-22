#include "render_planning.hpp"

#include "waydisplay/wd_config.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <utility>

namespace waydisplay {

uint64_t dirty_rect_pixel_count(const std::vector<ClientDirtyRect>& rects) {
    uint64_t pixels = 0;
    for (const ClientDirtyRect& rect : rects)
    {
        pixels += static_cast<uint64_t>(rect.w) * static_cast<uint64_t>(rect.h);
    }
    return pixels;
}

bool clamp_dirty_rect(const ClientDirtyRect& in, uint32_t frame_width, uint32_t frame_height, ClientDirtyRect& out) {
    if (in.w == 0 || in.h == 0 || in.x >= frame_width || in.y >= frame_height)
    {
        return false;
    }

    const uint32_t visible_width  = std::min<uint32_t>(in.w, frame_width - in.x);
    const uint32_t visible_height = std::min<uint32_t>(in.h, frame_height - in.y);
    if (visible_width == 0 || visible_height == 0 || visible_width > UINT16_MAX || visible_height > UINT16_MAX)
    {
        return false;
    }

    out.x = in.x;
    out.y = in.y;
    out.w = static_cast<uint16_t>(visible_width);
    out.h = static_cast<uint16_t>(visible_height);
    return true;
}


bool ClientDirtyTileGrid::reset(uint32_t frame_width, uint32_t frame_height,
                                uint16_t tile_width, uint16_t tile_height) {
    if (frame_width == 0 || frame_height == 0 || tile_width == 0 || tile_height == 0)
    {
        clear();
        frame_width_ = 0;
        frame_height_ = 0;
        tile_width_ = 0;
        tile_height_ = 0;
        tiles_x_ = 0;
        tiles_y_ = 0;
        dirty_tiles_.clear();
        previous_runs_.clear();
        current_runs_.clear();
        return false;
    }

    const uint32_t tiles_x = (frame_width + tile_width - 1u) / tile_width;
    const uint32_t tiles_y = (frame_height + tile_height - 1u) / tile_height;
    const uint64_t total_tiles = static_cast<uint64_t>(tiles_x) * tiles_y;
    if (tiles_x == 0 || tiles_y == 0 || total_tiles > static_cast<uint64_t>(SIZE_MAX))
    {
        return false;
    }

    frame_width_ = frame_width;
    frame_height_ = frame_height;
    tile_width_ = tile_width;
    tile_height_ = tile_height;
    tiles_x_ = tiles_x;
    tiles_y_ = tiles_y;
    dirty_tile_count_ = 0;
    dirty_tiles_.assign(static_cast<size_t>(total_tiles), 0);
    previous_runs_.clear();
    current_runs_.clear();
    previous_runs_.reserve(tiles_x_);
    current_runs_.reserve(tiles_x_);
    return true;
}

void ClientDirtyTileGrid::clear() {
    std::fill(dirty_tiles_.begin(), dirty_tiles_.end(), 0);
    dirty_tile_count_ = 0;
}

bool ClientDirtyTileGrid::mark_rect(const ClientDirtyRect& rect) {
    ClientDirtyRect visible{};
    if (dirty_tiles_.empty() || !clamp_dirty_rect(rect, frame_width_, frame_height_, visible))
    {
        return false;
    }

    const uint32_t tile_x0 = visible.x / tile_width_;
    const uint32_t tile_y0 = visible.y / tile_height_;
    const uint32_t tile_x1 = (static_cast<uint32_t>(visible.x) + visible.w - 1u) / tile_width_;
    const uint32_t tile_y1 = (static_cast<uint32_t>(visible.y) + visible.h - 1u) / tile_height_;

    for (uint32_t tile_y = tile_y0; tile_y <= tile_y1 && tile_y < tiles_y_; ++tile_y)
    {
        for (uint32_t tile_x = tile_x0; tile_x <= tile_x1 && tile_x < tiles_x_; ++tile_x)
        {
            uint8_t& dirty = dirty_tiles_[static_cast<size_t>(tile_y) * tiles_x_ + tile_x];
            if (dirty == 0)
            {
                dirty = 1;
                ++dirty_tile_count_;
            }
        }
    }
    return true;
}

void ClientDirtyTileGrid::take_rects(std::vector<ClientDirtyRect>& out_rects) {
    out_rects.clear();
    if (dirty_tile_count_ == 0 || dirty_tiles_.empty())
    {
        return;
    }

    previous_runs_.clear();
    current_runs_.clear();
    for (uint32_t tile_y = 0; tile_y < tiles_y_; ++tile_y)
    {
        current_runs_.clear();
        size_t previous_index = 0;
        uint32_t tile_x = 0;
        while (tile_x < tiles_x_)
        {
            const size_t bit_index = static_cast<size_t>(tile_y) * tiles_x_ + tile_x;
            if (dirty_tiles_[bit_index] == 0)
            {
                ++tile_x;
                continue;
            }

            const uint32_t run_start = tile_x;
            while (tile_x < tiles_x_)
            {
                uint8_t& dirty = dirty_tiles_[static_cast<size_t>(tile_y) * tiles_x_ + tile_x];
                if (dirty == 0)
                {
                    break;
                }
                dirty = 0;
                --dirty_tile_count_;
                ++tile_x;
            }
            const uint32_t run_end = tile_x;

            while (previous_index < previous_runs_.size() && previous_runs_[previous_index].start < run_start)
            {
                ++previous_index;
            }

            size_t rect_index = out_rects.size();
            if (previous_index < previous_runs_.size() && previous_runs_[previous_index].start == run_start &&
                previous_runs_[previous_index].end == run_end)
            {
                rect_index = previous_runs_[previous_index].rect_index;
                ClientDirtyRect& rect = out_rects[rect_index];
                const uint32_t bottom = std::min<uint32_t>(frame_height_, (tile_y + 1u) * tile_height_);
                rect.h = static_cast<uint16_t>(bottom - rect.y);
            }
            else
            {
                const uint32_t x = run_start * tile_width_;
                const uint32_t right = std::min<uint32_t>(frame_width_, run_end * tile_width_);
                const uint32_t y = tile_y * tile_height_;
                const uint32_t bottom = std::min<uint32_t>(frame_height_, y + tile_height_);
                out_rects.push_back({static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                     static_cast<uint16_t>(right - x), static_cast<uint16_t>(bottom - y)});
            }
            current_runs_.push_back({run_start, run_end, rect_index});
        }
        previous_runs_.swap(current_runs_);
    }
}

uint64_t ClientDirtyTileGrid::dirty_tile_count() const {
    return dirty_tile_count_;
}

bool configure_client_dirty_tile_grid(ClientDirtyTileGrid& grid, uint32_t frame_width, uint32_t frame_height,
                                      uint16_t tile_width, uint16_t tile_height) {
    return grid.reset(frame_width, frame_height, tile_width, tile_height);
}

ClientDirtyRect bounding_dirty_rect(const std::vector<ClientDirtyRect>& rects) {
    uint32_t min_x = UINT32_MAX;
    uint32_t min_y = UINT32_MAX;
    uint32_t max_x = 0;
    uint32_t max_y = 0;

    for (const ClientDirtyRect& rect : rects)
    {
        min_x = std::min<uint32_t>(min_x, rect.x);
        min_y = std::min<uint32_t>(min_y, rect.y);
        max_x = std::max<uint32_t>(max_x, static_cast<uint32_t>(rect.x) + rect.w);
        max_y = std::max<uint32_t>(max_y, static_cast<uint32_t>(rect.y) + rect.h);
    }

    ClientDirtyRect out{};
    if (min_x == UINT32_MAX || min_y == UINT32_MAX || max_x <= min_x || max_y <= min_y)
    {
        return out;
    }

    out.x = static_cast<uint16_t>(min_x);
    out.y = static_cast<uint16_t>(min_y);
    out.w = static_cast<uint16_t>(std::min<uint32_t>(max_x - min_x, UINT16_MAX));
    out.h = static_cast<uint16_t>(std::min<uint32_t>(max_y - min_y, UINT16_MAX));
    return out;
}

void coalesce_dirty_texture_rects(std::vector<ClientDirtyRect>& rects, uint32_t frame_width, uint32_t frame_height) {
    if (rects.empty())
    {
        return;
    }

    std::vector<ClientDirtyRect> clamped;
    clamped.reserve(rects.size());
    for (const ClientDirtyRect& rect : rects)
    {
        ClientDirtyRect visible{};
        if (clamp_dirty_rect(rect, frame_width, frame_height, visible))
        {
            clamped.push_back(visible);
        }
    }

    if (clamped.empty())
    {
        rects.clear();
        return;
    }

    std::sort(clamped.begin(), clamped.end(), [](const ClientDirtyRect& a, const ClientDirtyRect& b) {
        if (a.y != b.y)
        {
            return a.y < b.y;
        }
        if (a.h != b.h)
        {
            return a.h < b.h;
        }
        return a.x < b.x;
    });

    rects.clear();
    rects.reserve(clamped.size());
    for (const ClientDirtyRect& rect : clamped)
    {
        if (!rects.empty())
        {
            ClientDirtyRect& prev = rects.back();
            const uint32_t prev_end_x = static_cast<uint32_t>(prev.x) + prev.w;
            const uint32_t rect_end_x = static_cast<uint32_t>(rect.x) + rect.w;
            if (prev.y == rect.y && prev.h == rect.h && rect.x <= prev_end_x)
            {
                prev.w = static_cast<uint16_t>(std::min<uint32_t>(std::max(prev_end_x, rect_end_x) - prev.x, UINT16_MAX));
                continue;
            }
        }
        rects.push_back(rect);
    }

    /* Merge vertically adjacent row spans after the horizontal pass. */
    if (rects.size() > 1)
    {
        std::vector<ClientDirtyRect> horizontal = std::move(rects);
        std::sort(horizontal.begin(), horizontal.end(), [](const ClientDirtyRect& a, const ClientDirtyRect& b) {
            if (a.x != b.x)
            {
                return a.x < b.x;
            }
            if (a.w != b.w)
            {
                return a.w < b.w;
            }
            return a.y < b.y;
        });

        rects.clear();
        rects.reserve(horizontal.size());
        for (const ClientDirtyRect& rect : horizontal)
        {
            if (!rects.empty())
            {
                ClientDirtyRect& prev = rects.back();
                const uint32_t prev_end_y = static_cast<uint32_t>(prev.y) + prev.h;
                const uint32_t rect_end_y = static_cast<uint32_t>(rect.y) + rect.h;
                if (prev.x == rect.x && prev.w == rect.w && rect.y <= prev_end_y)
                {
                    prev.h = static_cast<uint16_t>(std::min<uint32_t>(std::max(prev_end_y, rect_end_y) - prev.y,
                                                                     UINT16_MAX));
                    continue;
                }
            }
            rects.push_back(rect);
        }
    }
}

namespace {

constexpr uint32_t COST_MODEL_MIN_SAMPLES = 4;
constexpr uint64_t DEFAULT_PIXEL_COST_Q16 = 1ull << 16u;
constexpr uint64_t DEFAULT_SNAPSHOT_PIXEL_COST_Q16 = 1ull << 16u;
constexpr uint64_t DEFAULT_UPDATE_CALL_COST_NS = 16384;
constexpr uint64_t DEFAULT_LOCK_CALL_COST_NS = 131072;

uint64_t scaled_pixel_cost_ns(uint64_t cost_q16, uint64_t pixels) {
    const __uint128_t scaled = static_cast<__uint128_t>(pixels) * cost_q16;
    const __uint128_t ns = scaled >> 16u;
    return ns > UINT64_MAX ? UINT64_MAX : static_cast<uint64_t>(ns);
}

uint64_t texture_pixel_cost_ns(const ClientTextureUploadCostModel& costs, uint64_t pixels) {
    const uint64_t cost_q16 = costs.pixel_samples >= COST_MODEL_MIN_SAMPLES ?
                                  costs.pixel_cost_q16 : DEFAULT_PIXEL_COST_Q16;
    return scaled_pixel_cost_ns(cost_q16, pixels);
}

uint64_t snapshot_pixel_cost_ns(const ClientTextureUploadCostModel& costs, uint64_t pixels) {
    const uint64_t cost_q16 = costs.snapshot_samples >= COST_MODEL_MIN_SAMPLES ?
                                  costs.snapshot_pixel_cost_q16 : DEFAULT_SNAPSHOT_PIXEL_COST_Q16;
    return scaled_pixel_cost_ns(cost_q16, pixels);
}

uint64_t update_call_cost_ns(const ClientTextureUploadCostModel& costs) {
    return costs.update_samples >= COST_MODEL_MIN_SAMPLES ?
               costs.update_call_cost_ns : DEFAULT_UPDATE_CALL_COST_NS;
}

uint64_t lock_call_cost_ns(const ClientTextureUploadCostModel& costs) {
    return costs.lock_samples >= COST_MODEL_MIN_SAMPLES ?
               costs.lock_call_cost_ns : DEFAULT_LOCK_CALL_COST_NS;
}

uint64_t saturating_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

uint64_t saturating_multiply(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0)
    {
        return 0;
    }
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

uint64_t bounded_ewma(uint64_t current, uint64_t sample, uint32_t samples) {
    if (samples == 0)
    {
        return sample;
    }
    if (samples >= COST_MODEL_MIN_SAMPLES && current != 0)
    {
        const uint64_t low = std::max<uint64_t>(1, current / 4u);
        const uint64_t high = current > UINT64_MAX / 4u ? UINT64_MAX : current * 4u;
        sample = std::clamp(sample, low, high);
    }
    return (current * 7ull + sample) / 8ull;
}

uint64_t upload_path_cost(const ClientTextureUploadCostModel& costs, uint64_t pixels,
                          uint64_t fixed_call_cost_ns, uint64_t call_count) {
    uint64_t cost = saturating_add(texture_pixel_cost_ns(costs, pixels),
                                   snapshot_pixel_cost_ns(costs, pixels));
    return saturating_add(cost, saturating_multiply(call_count, fixed_call_cost_ns));
}

bool materially_cheaper(uint64_t candidate, uint64_t current, uint32_t required_saving_percent) {
    if (current == 0)
    {
        return candidate == 0;
    }
    const __uint128_t candidate_scaled = static_cast<__uint128_t>(candidate) * 100u;
    const __uint128_t threshold = static_cast<__uint128_t>(current) * (100u - required_saving_percent);
    return candidate_scaled <= threshold;
}

} // namespace

void observe_texture_upload_call(ClientTextureUploadCostModel& model, bool texture_lock,
                                 uint64_t elapsed_ns, uint64_t pixels) {
    uint64_t& call_cost = texture_lock ? model.lock_call_cost_ns : model.update_call_cost_ns;
    uint32_t& call_samples = texture_lock ? model.lock_samples : model.update_samples;

    const uint64_t old_call_cost = call_cost;
    const uint64_t pixel_cost = scaled_pixel_cost_ns(model.pixel_cost_q16, pixels);
    const uint64_t fixed_sample = elapsed_ns > pixel_cost ? elapsed_ns - pixel_cost : 0;
    call_cost = bounded_ewma(call_cost, fixed_sample, call_samples);
    if (call_samples != UINT32_MAX)
    {
        ++call_samples;
    }

    if (pixels >= 64u * 1024u && elapsed_ns > old_call_cost)
    {
        const uint64_t variable_ns = elapsed_ns - old_call_cost;
        const __uint128_t scaled = static_cast<__uint128_t>(variable_ns) << 16u;
        uint64_t sample_q16 = static_cast<uint64_t>(scaled / pixels);
        sample_q16 = std::clamp<uint64_t>(sample_q16, 1u << 8u, 1000ull << 16u);
        model.pixel_cost_q16 = bounded_ewma(model.pixel_cost_q16, sample_q16, model.pixel_samples);
        if (model.pixel_samples != UINT32_MAX)
        {
            ++model.pixel_samples;
        }
    }
}

void observe_framebuffer_snapshot(ClientTextureUploadCostModel& model, uint64_t elapsed_ns, uint64_t pixels) {
    if (pixels == 0)
    {
        return;
    }
    const __uint128_t scaled = static_cast<__uint128_t>(elapsed_ns) << 16u;
    uint64_t sample_q16 = static_cast<uint64_t>(scaled / pixels);
    sample_q16 = std::clamp<uint64_t>(sample_q16, 1u << 8u, 1000ull << 16u);
    model.snapshot_pixel_cost_q16 = bounded_ewma(model.snapshot_pixel_cost_q16, sample_q16,
                                                 model.snapshot_samples);
    if (model.snapshot_samples != UINT32_MAX)
    {
        ++model.snapshot_samples;
    }
}

DirtyTextureUploadPlan plan_dirty_texture_upload(const std::vector<ClientDirtyRect>& rects,
                                                  uint32_t frame_width, uint32_t frame_height) {
    return plan_dirty_texture_upload(rects, frame_width, frame_height, ClientTextureUploadCostModel{});
}

DirtyTextureUploadPlan plan_dirty_texture_upload(const std::vector<ClientDirtyRect>& rects,
                                                  uint32_t frame_width, uint32_t frame_height,
                                                  const ClientTextureUploadCostModel& costs) {
    DirtyTextureUploadPlan plan{};
    plan.source_pixels = dirty_rect_pixel_count(rects);

    const uint64_t frame_pixels = static_cast<uint64_t>(frame_width) * static_cast<uint64_t>(frame_height);
    if (rects.empty() || frame_pixels == 0)
    {
        plan.mode = DirtyTextureUploadMode::Full;
        return plan;
    }

    if (rects.size() >= WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_THRESHOLD ||
        plan.source_pixels * 100ull >= frame_pixels * static_cast<uint64_t>(WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_PERCENT))
    {
        plan.mode = DirtyTextureUploadMode::Full;
        return plan;
    }

    uint64_t best_cost = upload_path_cost(costs, plan.source_pixels,
                                          update_call_cost_ns(costs), rects.size());

    const ClientDirtyRect bounds = bounding_dirty_rect(rects);
    if (bounds.w != 0 && bounds.h != 0)
    {
        const uint64_t bounds_pixels = static_cast<uint64_t>(bounds.w) * static_cast<uint64_t>(bounds.h);
        const uint64_t bounds_cost = upload_path_cost(costs, bounds_pixels, lock_call_cost_ns(costs), 1);
        if (materially_cheaper(bounds_cost, best_cost, 15))
        {
            plan.mode = DirtyTextureUploadMode::Bounds;
            plan.bounds = bounds;
            best_cost = bounds_cost;
        }
    }

    const uint64_t full_cost = upload_path_cost(costs, frame_pixels, lock_call_cost_ns(costs), 1);
    if (materially_cheaper(full_cost, best_cost, 20))
    {
        plan.mode = DirtyTextureUploadMode::Full;
    }

    return plan;
}



void claim_pending_tile_generations(std::vector<uint64_t>& pending,
                                    const std::vector<uint64_t>& presented,
                                    const std::vector<uint16_t>& tile_ids,
                                    std::vector<ClientTileGenerationUpdate>& out_updates) {
    out_updates.clear();
    if (pending.size() != presented.size())
    {
        return;
    }

    out_updates.reserve(tile_ids.size());
    for (uint16_t tile_id : tile_ids)
    {
        if (tile_id >= pending.size())
        {
            continue;
        }
        const uint64_t generation = pending[tile_id];
        if (generation > presented[tile_id])
        {
            out_updates.push_back({tile_id, generation});
        }
        if (generation != 0)
        {
            pending[tile_id] = 0;
        }
    }
}

void claim_all_pending_tile_generations(std::vector<uint64_t>& pending,
                                        const std::vector<uint64_t>& presented,
                                        std::vector<ClientTileGenerationUpdate>& out_updates) {
    out_updates.clear();
    if (pending.size() != presented.size())
    {
        return;
    }

    out_updates.reserve(pending.size());
    for (size_t tile_id = 0; tile_id < pending.size(); ++tile_id)
    {
        const uint64_t generation = pending[tile_id];
        if (generation > presented[tile_id])
        {
            out_updates.push_back({static_cast<uint16_t>(tile_id), generation});
        }
        if (generation != 0)
        {
            pending[tile_id] = 0;
        }
    }
}

void requeue_tile_generation_updates(std::vector<uint64_t>& pending,
                                     const std::vector<ClientTileGenerationUpdate>& updates) {
    for (const ClientTileGenerationUpdate& update : updates)
    {
        if (update.tile_id < pending.size() && pending[update.tile_id] < update.generation)
        {
            pending[update.tile_id] = update.generation;
        }
    }
}

void commit_tile_generation_updates(std::vector<uint64_t>& presented,
                                    const std::vector<ClientTileGenerationUpdate>& updates) {
    for (const ClientTileGenerationUpdate& update : updates)
    {
        if (update.tile_id < presented.size() && presented[update.tile_id] < update.generation)
        {
            presented[update.tile_id] = update.generation;
        }
    }
}


bool summary_pending_index_add(std::vector<uint16_t>& active_tiles,
                               std::vector<uint32_t>& positions, uint16_t tile_id) {
    if (tile_id >= positions.size())
    {
        return false;
    }
    if (positions[tile_id] != UINT32_MAX)
    {
        return true;
    }
    if (active_tiles.size() >= UINT32_MAX)
    {
        return false;
    }
    positions[tile_id] = static_cast<uint32_t>(active_tiles.size());
    active_tiles.push_back(tile_id);
    return true;
}

bool summary_pending_index_remove(std::vector<uint16_t>& active_tiles,
                                  std::vector<uint32_t>& positions, uint16_t tile_id) {
    if (tile_id >= positions.size())
    {
        return false;
    }
    const uint32_t position = positions[tile_id];
    if (position == UINT32_MAX)
    {
        return true;
    }
    if (position >= active_tiles.size())
    {
        return false;
    }
    const uint16_t moved_tile = active_tiles.back();
    active_tiles[position] = moved_tile;
    positions[moved_tile] = position;
    active_tiles.pop_back();
    positions[tile_id] = UINT32_MAX;
    return true;
}

bool should_collect_pending_tile_dirty(bool texture_needs_full_upload, bool video_frame_pending) {
    return !texture_needs_full_upload && !video_frame_pending;
}

bool should_clear_pending_tile_dirty(bool full_framebuffer_uploaded, bool video_frame_uploaded) {
    return full_framebuffer_uploaded && !video_frame_uploaded;
}

} // namespace waydisplay
