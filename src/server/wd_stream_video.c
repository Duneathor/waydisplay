#include "wd_stream_pipeline_internal.h"

#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_time.h"
#include "wd_async_tcp.h"
#include "wd_video_encoder.h"
#include "wd_video_transition.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool wd_stream_video_frame_due_locked(const struct wd_stream_policy* policy, uint64_t now_ns) {
    if (!policy)
    {
        return false;
    }

    uint16_t fps = wd_stream_policy_effective_fps_locked(policy);
    if (fps == 0)
    {
        fps = WD_DEFAULT_CAPTURE_FPS;
    }

    const uint64_t interval_ns = WD_NSEC_PER_SEC / fps;
    return policy->last_video_frame_send_ns == 0 || now_ns - policy->last_video_frame_send_ns >= interval_ns;
}

uint32_t wd_stream_video_bitrate_kib_locked(const struct wd_stream_policy* policy) {
    if (policy && policy->video_bitrate_kib_per_second != 0)
    {
        return policy->video_bitrate_kib_per_second;
    }

    if (!policy || policy->video_bytes_per_second == 0)
    {
        return WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND;
    }

    uint64_t kib = policy->video_bytes_per_second / 1024ull;
    if (kib == 0)
    {
        return WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND;
    }

    if (kib > WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND)
    {
        kib = WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND;
    }

    return (uint32_t)kib;
}

struct wd_video_worker_job {
    uint32_t* pixels;
    size_t    pixel_capacity;

    struct wd_video_encoder_config config;
    uint64_t                       epoch;
    uint64_t                       source_content_epoch;
    uint64_t                       published_ns;
    uint64_t                       pts_usec;
    int                            video_tcp_fd;
    bool                           request_keyframe;
};

struct wd_video_worker {
    struct wd_server* server;
    pthread_t         thread;
    pthread_mutex_t   lock;
    pthread_cond_t    cond;
    bool              thread_started;
    bool              stop;
    bool              pending;

    struct wd_video_worker_job pending_job;
    struct wd_video_worker_job active_job;
};

static bool wd_stream_video_job_current_locked(const struct wd_server* server, const struct wd_video_worker_job* job) {
    if (!server || !job)
    {
        return false;
    }

    const struct wd_net_state* net = &server->net;
    return wd_net_run_state_is_running(&net->run_state) && net->video_worker_epoch == job->epoch &&
           net->session_id == job->config.session_id && net->connection_token == job->config.connection_token &&
           net->content_epoch == job->source_content_epoch && net->video_tcp_fd == job->video_tcp_fd && net->video_stream_negotiated &&
           net->video_tx &&
           (net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY || net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_ACTIVE);
}

static void wd_stream_video_worker_process(struct wd_video_worker* worker, struct wd_video_worker_job* job) {
    if (!worker || !worker->server || !job || !job->pixels)
    {
        return;
    }

    struct wd_server*    server = worker->server;
    struct wd_net_state* net    = &server->net;

    const uint64_t dequeue_ns = wd_now_ns();

    pthread_mutex_lock(&net->lock);
    if (!wd_stream_video_job_current_locked(server, job))
    {
        net->stats.video_worker_stale_drops++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    wd_async_tcp_sender_reap(net->video_tx);
    if (wd_async_tcp_sender_has_message_type(net->video_tx, WD_MSG_VIDEO_FRAME))
    {
        net->stats.video_keyframe_skipped_pending++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    net->stats.video_frame_attempts++;
    if (job->request_keyframe)
    {
        net->stats.video_keyframe_attempts++;
    }
    if (dequeue_ns >= job->published_ns)
    {
        net->stats.video_worker_queue_samples++;
        net->stats.video_worker_queue_ns += dequeue_ns - job->published_ns;
    }
    pthread_mutex_unlock(&net->lock);

    struct wd_video_encoder_input_xrgb8888 input;
    memset(&input, 0, sizeof(input));
    input.pixels        = job->pixels;
    input.width         = job->config.width;
    input.height        = job->config.height;
    input.stride_pixels = job->config.width;
    input.pts_usec      = job->pts_usec;

    struct wd_video_encoder_packet packet;
    memset(&packet, 0, sizeof(packet));

    const uint64_t                       encode_start_ns = wd_now_ns();
    bool                                 encoded         = false;
    bool                                 no_output       = false;
    bool                                 payload_invalid = false;
    uint8_t*                             payload         = NULL;
    uint32_t                             payload_size    = 0;
    struct wd_video_frame_payload_header header;
    memset(&header, 0, sizeof(header));
    struct wd_video_entry_plan entry_plan = wd_video_entry_plan_make(job->source_content_epoch, false, false);

    pthread_mutex_lock(&net->video_encoder_lock);
    if (wd_video_encoder_configure(net->video_encoder, &job->config) &&
        (!job->request_keyframe || wd_video_encoder_request_keyframe(net->video_encoder)))
    {
        encoded   = wd_video_encoder_encode_xrgb8888(net->video_encoder, &input, &packet);
        no_output = encoded && (!packet.data || packet.header.data_size == 0);
        if (encoded && !no_output)
        {
            header                  = packet.header;
            header.session_id       = job->config.session_id;
            header.connection_token = job->config.connection_token;
            entry_plan =
                wd_video_entry_plan_make(job->source_content_epoch, job->request_keyframe, (header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
            header.content_epoch = entry_plan.frame_content_epoch;
            header.codec         = job->config.codec;
            header.pts_usec      = input.pts_usec;
            header.width         = job->config.width;
            header.height        = job->config.height;
            if (header.coded_width == 0)
            {
                header.coded_width = header.width;
            }
            if (header.coded_height == 0)
            {
                header.coded_height = header.height;
            }

            const uint64_t payload_size64 = (uint64_t)sizeof(header) + (uint64_t)header.data_size;
            if (header.data_size > WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES || payload_size64 > UINT32_MAX)
            {
                payload_invalid = true;
            }
            else
            {
                payload = malloc((size_t)payload_size64);
                if (!payload)
                {
                    payload_invalid = true;
                }
                else
                {
                    payload_size = (uint32_t)payload_size64;
                    memcpy(payload, &header, sizeof(header));
                    memcpy(payload + sizeof(header), packet.data, header.data_size);
                }
            }
        }
    }
    pthread_mutex_unlock(&net->video_encoder_lock);

    const uint64_t encode_ns = wd_now_ns() - encode_start_ns;

    pthread_mutex_lock(&net->lock);
    net->stats.video_encode_ns += encode_ns;

    if (!encoded || payload_invalid)
    {
        free(payload);
        net->stats.video_encode_failed++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    if (no_output)
    {
        pthread_mutex_unlock(&net->lock);
        return;
    }

    if (!wd_stream_video_job_current_locked(server, job))
    {
        free(payload);
        net->stats.video_worker_stale_drops++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    wd_async_tcp_sender_reap(net->video_tx);
    if (wd_async_tcp_sender_has_message_type(net->video_tx, WD_MSG_VIDEO_FRAME))
    {
        free(payload);
        net->stats.video_keyframe_skipped_pending++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    if (job->request_keyframe && !entry_plan.commit_on_queue)
    {
        free(payload);
        net->stats.video_encode_failed++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    const bool queued = wd_async_tcp_send_message(net->video_tx, net->video_tcp_fd, WD_MSG_VIDEO_FRAME, payload, payload_size);
    free(payload);

    if (!queued)
    {
        net->stats.video_tcp_send_failed++;
        wd_stream_video_reset_locked(server, "video tcp send failed", false, false);
        pthread_mutex_unlock(&net->lock);
        return;
    }

    net->stats.video_frames_tx++;
    if ((header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0)
    {
        net->stats.video_keyframes_tx++;
    }
    net->stats.video_tcp_bytes_tx += payload_size;

    if (net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY &&
        wd_video_entry_plan_can_commit(&entry_plan, net->content_epoch, true))
    {
        net->content_epoch                       = entry_plan.frame_content_epoch;
        net->input_correlation_inflight_sequence = 0;
        WD_LOG_INFO("stream content epoch: epoch=%llu reason=first video keyframe queued", (unsigned long long)net->content_epoch);
        wd_stream_policy_set_mode_locked(&net->stream_policy, WD_STREAM_MODE_VIDEO_ACTIVE, WD_VIDEO_RECOVERY_NONE, "video keyframe queued", 0.0, 0.0, 0.0, true,
                                         true);
    }

    pthread_mutex_unlock(&net->lock);
}

static void* wd_stream_video_worker_main(void* data) {
    struct wd_video_worker* worker = data;
    if (!worker)
    {
        return NULL;
    }

    for (;;)
    {
        pthread_mutex_lock(&worker->lock);
        while (!worker->stop && !worker->pending)
        {
            pthread_cond_wait(&worker->cond, &worker->lock);
        }

        if (worker->stop)
        {
            pthread_mutex_unlock(&worker->lock);
            break;
        }

        struct wd_video_worker_job tmp = worker->active_job;
        worker->active_job             = worker->pending_job;
        worker->pending_job            = tmp;
        worker->pending                = false;
        pthread_mutex_unlock(&worker->lock);

        wd_stream_video_worker_process(worker, &worker->active_job);
    }

    return NULL;
}

bool wd_stream_video_worker_init(struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    if (!wd_video_encoder_available(net->video_encoder))
    {
        return true;
    }
    if (net->video_worker)
    {
        return true;
    }

    struct wd_video_worker* worker = calloc(1, sizeof(*worker));
    if (!worker)
    {
        return false;
    }
    worker->server = server;

    if (pthread_mutex_init(&worker->lock, NULL) != 0)
    {
        free(worker);
        return false;
    }
    if (pthread_cond_init(&worker->cond, NULL) != 0)
    {
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return false;
    }

    net->video_worker_epoch = 1;
    net->video_worker       = worker;
    if (pthread_create(&worker->thread, NULL, wd_stream_video_worker_main, worker) != 0)
    {
        net->video_worker = NULL;
        pthread_cond_destroy(&worker->cond);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return false;
    }
    worker->thread_started = true;
    return true;
}

void wd_stream_video_worker_destroy(struct wd_server* server) {
    if (!server || !server->net.video_worker)
    {
        return;
    }

    struct wd_video_worker* worker = server->net.video_worker;
    server->net.video_worker       = NULL;

    pthread_mutex_lock(&worker->lock);
    worker->stop    = true;
    worker->pending = false;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    if (worker->thread_started)
    {
        pthread_join(worker->thread, NULL);
    }

    free(worker->pending_job.pixels);
    free(worker->active_job.pixels);
    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->lock);
    free(worker);
}

static void wd_stream_video_worker_discard_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    net->video_worker_epoch++;
    if (net->video_worker_epoch == 0)
    {
        net->video_worker_epoch = 1;
    }

    struct wd_video_worker* worker = net->video_worker;
    if (!worker)
    {
        return;
    }

    pthread_mutex_lock(&worker->lock);
    if (worker->pending)
    {
        worker->pending = false;
        net->stats.video_worker_stale_drops++;
    }
    pthread_mutex_unlock(&worker->lock);
}

bool wd_stream_queue_video_control_frame_locked(struct wd_server* server, uint16_t flags) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    if (!net->video_stream_negotiated || net->video_tcp_fd < 0 || !net->video_tx)
    {
        return false;
    }

    struct wd_video_frame_payload_header header;
    memset(&header, 0, sizeof(header));
    header.session_id       = net->session_id;
    header.connection_token = net->connection_token;
    header.content_epoch    = net->content_epoch;
    header.codec            = net->video_codecs != 0 ? net->video_codecs : WD_VIDEO_CODEC_H265;
    header.flags            = flags;
    header.pts_usec         = wd_media_ns_to_usec(wd_now_ns(), net->media_clock_start_ns);
    header.width            = (uint16_t)server->display_width;
    header.height           = (uint16_t)server->display_height;
    header.coded_width      = (uint16_t)server->display_width;
    header.coded_height     = (uint16_t)server->display_height;

    const bool queued = wd_async_tcp_send_message(net->video_tx, net->video_tcp_fd, WD_MSG_VIDEO_FRAME, &header, (uint32_t)sizeof(header));
    if (!queued)
    {
        net->stats.video_tcp_send_failed++;
        return false;
    }

    net->stats.video_control_frames_tx++;
    net->stats.video_tcp_bytes_tx += sizeof(header);
    if ((flags & WD_VIDEO_FRAME_END_OF_STREAM) != 0)
    {
        net->stats.video_end_of_stream_tx++;
    }
    return true;
}

void wd_stream_video_reset_locked(struct wd_server* server, const char* reason, bool notify_client, bool resize) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net           = &server->net;
    const bool           leaving_video = wd_stream_mode_video_owns_display(net->stream_policy.stream_mode);
    if (resize || leaving_video)
    {
        wd_stream_advance_content_epoch_locked(server, reason ? reason : "video reset");
    }
    if (net->video_tx)
    {
        (void)wd_async_tcp_sender_drop_message_type(net->video_tx, WD_MSG_VIDEO_FRAME);
    }

    if (notify_client)
    {
        uint16_t flags = WD_VIDEO_FRAME_END_OF_STREAM;
        if (resize)
        {
            flags |= WD_VIDEO_FRAME_RESIZE;
        }
        (void)wd_stream_queue_video_control_frame_locked(server, flags);
    }

    /* Invalidate both the pending mailbox frame and any frame currently being
     * encoded. The active worker will validate the epoch before it queues its
     * packet, so reset/resize cannot publish stale output. */
    wd_stream_video_worker_discard_locked(server);

    pthread_mutex_lock(&net->video_encoder_lock);
    wd_video_encoder_reset(net->video_encoder);
    pthread_mutex_unlock(&net->video_encoder_lock);
    net->stream_policy.last_video_frame_send_ns = 0;
    net->stream_policy.video_candidate_seconds  = 0;
    net->stream_policy.tile_recovery_seconds    = 0;
    net->stats.video_resets++;
    if (resize)
    {
        net->stats.video_resize_resets++;
    }

    if (net->stream_policy.stream_mode != WD_STREAM_MODE_TILES)
    {
        wd_stream_policy_set_mode_locked(&net->stream_policy, net->client_connected ? WD_STREAM_MODE_TILE_RECOVERY : WD_STREAM_MODE_TILES,
                                         net->client_connected ? (resize ? WD_VIDEO_RECOVERY_PLANNED : WD_VIDEO_RECOVERY_FAILURE)
                                                               : WD_VIDEO_RECOVERY_NONE,
                                         reason ? reason : "video reset", 0.0, 0.0, 0.0, net->video_tcp_fd >= 0,
                                         wd_video_encoder_available(net->video_encoder));
    }
    else if (reason)
    {
        WD_LOG_INFO("video stream reset: reason=%s notify_client=%s resize=%s video_channel=%s", reason, notify_client ? "yes" : "no",
                    resize ? "yes" : "no", net->video_tcp_fd >= 0 ? "yes" : "no");
    }
}

bool wd_stream_video_snapshot_needed(struct wd_server* server, uint64_t now_ns) {
    if (!server || !server->framebuffer_xrgb8888)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    pthread_mutex_lock(&net->lock);
    const bool needed = net->video_worker &&
                        (net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY ||
                         net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_ACTIVE) &&
                        net->video_stream_negotiated && net->video_tcp_fd >= 0 && net->video_tx &&
                        wd_video_encoder_available(net->video_encoder) &&
                        wd_stream_video_frame_due_locked(&net->stream_policy, now_ns);
    pthread_mutex_unlock(&net->lock);
    return needed;
}

bool wd_stream_try_publish_video_snapshot_locked(struct wd_server* server, uint64_t now_ns,
                                                  struct wd_stream_video_snapshot* snapshot) {
    if (!server || !snapshot || !snapshot->ready || !snapshot->pixels)
    {
        return false;
    }

    struct wd_net_state*    net    = &server->net;
    struct wd_video_worker* worker = net->video_worker;
    if (!worker ||
        (net->stream_policy.stream_mode != WD_STREAM_MODE_VIDEO_READY && net->stream_policy.stream_mode != WD_STREAM_MODE_VIDEO_ACTIVE) ||
        !net->video_stream_negotiated || net->video_tcp_fd < 0 || !net->video_tx || !wd_video_encoder_available(net->video_encoder))
    {
        return false;
    }

    wd_async_tcp_sender_reap(net->video_tx);
    if (wd_async_tcp_sender_has_message_type(net->video_tx, WD_MSG_VIDEO_FRAME))
    {
        net->stats.video_keyframe_skipped_pending++;
        return false;
    }

    if (!wd_stream_video_frame_due_locked(&net->stream_policy, now_ns))
    {
        return false;
    }

    const uint32_t width  = server->display_width;
    const uint32_t height = server->display_height;
    if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
    {
        net->stats.video_encode_failed++;
        return false;
    }

    const size_t pixel_count = (size_t)width * (size_t)height;
    if ((height != 0 && pixel_count / height != width) || pixel_count > SIZE_MAX / sizeof(uint32_t) ||
        snapshot->pixel_count != pixel_count || snapshot->pixel_capacity < pixel_count)
    {
        net->stats.video_encode_failed++;
        return false;
    }

    struct wd_video_encoder_config config;
    memset(&config, 0, sizeof(config));
    config.session_id             = net->session_id;
    config.connection_token       = net->connection_token;
    config.content_epoch          = net->content_epoch;
    config.width                  = (uint16_t)width;
    config.height                 = (uint16_t)height;
    config.target_fps             = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    config.bitrate_kib_per_second = wd_stream_video_bitrate_kib_locked(&net->stream_policy);
    config.codec                  = net->video_codecs != 0 ? net->video_codecs : WD_VIDEO_CODEC_H265;

    const bool request_keyframe = net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY;

    pthread_mutex_lock(&worker->lock);
    if (worker->stop)
    {
        pthread_mutex_unlock(&worker->lock);
        return false;
    }

    const bool superseded = worker->pending;
    uint32_t*    spare_pixels   = worker->pending_job.pixels;
    const size_t spare_capacity = worker->pending_job.pixel_capacity;
    worker->pending_job.pixels         = snapshot->pixels;
    worker->pending_job.pixel_capacity = snapshot->pixel_capacity;
    snapshot->pixels                   = spare_pixels;
    snapshot->pixel_capacity           = spare_capacity;
    snapshot->pixel_count              = 0;
    snapshot->ready                    = false;

    worker->pending_job.config               = config;
    worker->pending_job.epoch                = net->video_worker_epoch;
    worker->pending_job.source_content_epoch = net->content_epoch;
    worker->pending_job.published_ns         = now_ns;
    worker->pending_job.pts_usec             = wd_media_ns_to_usec(now_ns, net->media_clock_start_ns);
    worker->pending_job.video_tcp_fd         = net->video_tcp_fd;
    worker->pending_job.request_keyframe     = request_keyframe;
    worker->pending                          = true;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    net->stats.video_frames_published++;
    if (superseded)
    {
        net->stats.video_frames_superseded++;
    }
    net->stats.video_publish_copy_samples++;
    net->stats.video_publish_copy_ns += snapshot->copy_ns;
    snapshot->copy_ns = 0;

    /* This timestamp paces capture/publication, not TCP completion. The worker
     * may discard an older pending frame when a fresher one arrives. */
    net->stream_policy.last_video_frame_send_ns = now_ns;
    return true;
}
