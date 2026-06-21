#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace waydisplay {

struct ClientDirtyRect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

class ClientDirtyTileGrid {
  public:
    bool reset(uint32_t frame_width, uint32_t frame_height, uint16_t tile_width, uint16_t tile_height);
    void clear();
    bool mark_rect(const ClientDirtyRect& rect);
    void take_rects(std::vector<ClientDirtyRect>& out_rects);
    uint64_t dirty_tile_count() const;

  private:
    struct RowRun {
        uint32_t start = 0;
        uint32_t end = 0;
        size_t   rect_index = 0;
    };

    uint32_t             frame_width_ = 0;
    uint32_t             frame_height_ = 0;
    uint16_t             tile_width_ = 0;
    uint16_t             tile_height_ = 0;
    uint32_t             tiles_x_ = 0;
    uint32_t             tiles_y_ = 0;
    uint64_t             dirty_tile_count_ = 0;
    std::vector<uint8_t> dirty_tiles_;
    std::vector<RowRun>  previous_runs_;
    std::vector<RowRun>  current_runs_;
};

enum class DirtyTextureUploadMode : uint8_t {
    Rects,
    Bounds,
    Full,
};

struct DirtyTextureUploadPlan {
    DirtyTextureUploadMode mode = DirtyTextureUploadMode::Rects;
    ClientDirtyRect        bounds{};
    uint64_t               source_pixels = 0;
};
struct ClientTextureUploadCostModel {
    uint64_t pixel_cost_q16 = 1ull << 16u;
    uint64_t update_call_cost_ns = 16384;
    uint64_t lock_call_cost_ns = 131072;
    uint32_t update_samples = 0;
    uint32_t lock_samples = 0;
    uint32_t pixel_samples = 0;
};

void observe_texture_upload_call(ClientTextureUploadCostModel& model, bool texture_lock,
                                 uint64_t elapsed_ns, uint64_t pixels);


uint64_t dirty_rect_pixel_count(const std::vector<ClientDirtyRect>& rects);
bool clamp_dirty_rect(const ClientDirtyRect& in, uint32_t frame_width, uint32_t frame_height, ClientDirtyRect& out);
ClientDirtyRect bounding_dirty_rect(const std::vector<ClientDirtyRect>& rects);
void coalesce_dirty_texture_rects(std::vector<ClientDirtyRect>& rects, uint32_t frame_width, uint32_t frame_height);
DirtyTextureUploadPlan plan_dirty_texture_upload(const std::vector<ClientDirtyRect>& rects,
                                                  uint32_t frame_width, uint32_t frame_height);
DirtyTextureUploadPlan plan_dirty_texture_upload(const std::vector<ClientDirtyRect>& rects,
                                                  uint32_t frame_width, uint32_t frame_height,
                                                  const ClientTextureUploadCostModel& costs);
enum class FramebufferUploadPath : uint8_t {
    Direct,
    Staged,
};

FramebufferUploadPath choose_framebuffer_upload_path(const DirtyTextureUploadPlan& plan,
                                                      size_t rect_count,
                                                      uint64_t recent_lock_wait_ns);


struct ClientTileGenerationUpdate {
    uint16_t tile_id = 0;
    uint64_t generation = 0;
};

void claim_pending_tile_generations(std::vector<uint64_t>& pending,
                                    const std::vector<uint64_t>& presented,
                                    const std::vector<uint16_t>& tile_ids,
                                    std::vector<ClientTileGenerationUpdate>& out_updates);
void claim_all_pending_tile_generations(std::vector<uint64_t>& pending,
                                        const std::vector<uint64_t>& presented,
                                        std::vector<ClientTileGenerationUpdate>& out_updates);
void requeue_tile_generation_updates(std::vector<uint64_t>& pending,
                                     const std::vector<ClientTileGenerationUpdate>& updates);
void commit_tile_generation_updates(std::vector<uint64_t>& presented,
                                    const std::vector<ClientTileGenerationUpdate>& updates);
bool summary_pending_index_add(std::vector<uint16_t>& active_tiles,
                               std::vector<uint32_t>& positions, uint16_t tile_id);
bool summary_pending_index_remove(std::vector<uint16_t>& active_tiles,
                                  std::vector<uint32_t>& positions, uint16_t tile_id);


bool should_collect_pending_tile_dirty(bool texture_needs_full_upload, bool video_frame_pending);
bool should_clear_pending_tile_dirty(bool full_framebuffer_uploaded, bool video_frame_uploaded);

} // namespace waydisplay
