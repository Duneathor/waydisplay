#include "wd_stream_pipeline_internal.h"

#include "wd_video_encoder.h"

void wd_stream_controller_tick(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    pthread_mutex_lock(&net->lock);

    const bool video_owned_before = wd_stream_mode_video_owns_display(net->stream_policy.stream_mode);
    wd_stream_policy_update_health_locked(&net->stream_policy, &net->stats);
    wd_stream_policy_update_mode_locked(&net->stream_policy, &net->stats, server->total_tiles, net->video_stream_negotiated,
                                        net->video_tcp_fd >= 0, wd_video_encoder_available(net->video_encoder));
    const bool video_owned_after = wd_stream_mode_video_owns_display(net->stream_policy.stream_mode);
    if (video_owned_before != video_owned_after)
    {
        wd_stream_advance_content_epoch_locked(server, video_owned_after ? "video owns display" : "tiles own display");
    }

    pthread_mutex_unlock(&net->lock);
}
