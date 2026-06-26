#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_content_owner {
    WD_CLIENT_CONTENT_OWNER_TILES = 0,
    WD_CLIENT_CONTENT_OWNER_VIDEO = 1,
};

struct wd_client_content_ownership_snapshot {
    uint64_t                     epoch;
    enum wd_client_content_owner owner;
};

struct wd_client_stream_ownership {
    uint64_t packed_state;
};

#define WD_CLIENT_STREAM_OWNERSHIP_INITIAL_STATE UINT64_C(2)
#define WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER   {WD_CLIENT_STREAM_OWNERSHIP_INITIAL_STATE}

void wd_client_stream_ownership_init(struct wd_client_stream_ownership* ownership);
struct wd_client_content_ownership_snapshot
wd_client_stream_ownership_snapshot(const struct wd_client_stream_ownership* ownership);
uint64_t wd_client_stream_ownership_begin_video_stream(struct wd_client_stream_ownership* ownership);
uint64_t wd_client_stream_ownership_end_video_stream(struct wd_client_stream_ownership* ownership);
uint64_t wd_client_stream_ownership_reset_to_video(struct wd_client_stream_ownership* ownership);
uint64_t wd_client_stream_ownership_reset_to_tiles(struct wd_client_stream_ownership* ownership);
bool wd_client_stream_ownership_is_current(const struct wd_client_stream_ownership* ownership, uint64_t epoch,
                                           enum wd_client_content_owner owner);

#ifdef __cplusplus
}
#endif
