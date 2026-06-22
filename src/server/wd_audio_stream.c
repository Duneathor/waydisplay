#include "wd_audio_stream.h"

#include "wd_async_tcp.h"
#include "wd_audio_capture.h"
#include "wd_audio_encoder.h"
#include "wd_audio_packetizer.h"
#include "wd_audio_ring.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_protocol.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#define WD_AUDIO_CAPTURE_RING_MS 200u
#define WD_AUDIO_TX_QUEUE_MS 100u
#define WD_AUDIO_TX_MIN_PENDING_BYTES 4096u
#define WD_AUDIO_WORKER_SLEEP_US 2000u
#define WD_AUDIO_CAPTURE_NSEC_TOLERANCE_CYCLES 2u
#define WD_AUDIO_CAPTURE_IDLE_DIAGNOSTIC_NS (10ull * 1000ull * 1000ull * 1000ull)


static uint64_t wd_audio_tx_max_pending_bytes(uint32_t bitrate) {
    const uint64_t payload = ((uint64_t)bitrate * WD_AUDIO_TX_QUEUE_MS + 7999u) / 8000u;
    const uint64_t packets = (WD_AUDIO_TX_QUEUE_MS + 19u) / 20u;
    const uint64_t overhead = packets *
        (sizeof(struct wd_tcp_header) + sizeof(struct wd_audio_packet_payload_header));
    const uint64_t total = payload + overhead;
    return total < WD_AUDIO_TX_MIN_PENDING_BYTES ? WD_AUDIO_TX_MIN_PENDING_BYTES : total;
}

static void wd_audio_limit_socket_send_buffer(int fd, uint64_t queue_bytes) {
    if (fd < 0)
    {
        return;
    }
    int requested = queue_bytes > INT32_MAX ? INT32_MAX : (int)queue_bytes;
    if (requested < (int)WD_AUDIO_TX_MIN_PENDING_BYTES)
    {
        requested = (int)WD_AUDIO_TX_MIN_PENDING_BYTES;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested));
}

static void wd_audio_sleep_us(unsigned usec) {
    struct timespec ts;
    ts.tv_sec = (time_t)(usec / 1000000u);
    ts.tv_nsec = (long)(usec % 1000000u) * 1000L;
    while (nanosleep(&ts, &ts) != 0)
    {
    }
}

struct wd_audio_stream {
    pthread_mutex_t lock;
    pthread_t worker;
    bool worker_started;
    bool running;
    bool stop_requested;
    bool capture_started;
    bool force_discontinuity;
    bool capture_delivery_enabled;
    uint32_t capture_callbacks_active;
    bool capture_active_logged;
    bool capture_idle_logged;
    uint64_t capture_started_ns;

    int tcp_fd;
    uint8_t channels;
    uint32_t bitrate;
    uint16_t target_latency_ms;
    uint64_t tx_max_pending_bytes;
    uint64_t media_clock_start_ns;
    uint64_t capture_next_pts;
    uint64_t capture_clock_origin_position;
    uint64_t capture_clock_origin_pts;
    uint32_t capture_clock_id;
    uint32_t capture_clock_rate_num;
    uint32_t capture_clock_rate_denom;
    bool capture_pts_valid;
    bool capture_clock_valid;

    struct wd_audio_capture* capture;
    struct wd_audio_encoder* encoder;
    struct wd_async_tcp_sender* tx;
    float* pcm_buffer;
    uint8_t* packet_buffer;
    uint8_t* payload_buffer;
    struct wd_audio_pcm_ring ring;
    struct wd_audio_packetizer packetizer;
    struct wd_audio_stream_stats stats;
};

static uint64_t wd_audio_graph_ticks_to_samples(uint64_t ticks, uint32_t rate_num,
                                                 uint32_t rate_denom) {
    if (rate_num == 0 || rate_denom == 0)
    {
        return 0;
    }
    const __uint128_t scaled = (__uint128_t)ticks * WD_AUDIO_SAMPLE_RATE_DEFAULT *
                               rate_num;
    const __uint128_t samples = scaled / rate_denom;
    return samples > UINT64_MAX ? UINT64_MAX : (uint64_t)samples;
}

static uint64_t wd_audio_abs_difference(uint64_t a, uint64_t b) {
    return a >= b ? a - b : b - a;
}

static void wd_audio_stream_capture(
    void* userdata, const float* samples, uint32_t frames, uint8_t channels,
    const struct wd_audio_capture_timing* timing) {
    struct wd_audio_stream* stream = userdata;
    if (!stream || !samples || !timing || frames == 0 || channels < stream->channels ||
        !__atomic_load_n(&stream->capture_delivery_enabled, __ATOMIC_ACQUIRE))
    {
        return;
    }

    __atomic_add_fetch(&stream->capture_callbacks_active, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&stream->capture_delivery_enabled, __ATOMIC_ACQUIRE))
    {
        __atomic_sub_fetch(&stream->capture_callbacks_active, 1, __ATOMIC_ACQ_REL);
        return;
    }

    const uint64_t observed_first_pts = wd_media_ns_to_samples(
        timing->cycle_start_ns, stream->media_clock_start_ns,
        WD_AUDIO_SAMPLE_RATE_DEFAULT);
    const uint64_t tolerance = (uint64_t)frames *
                               WD_AUDIO_CAPTURE_NSEC_TOLERANCE_CYCLES;
    uint64_t first_pts = observed_first_pts;

    if (timing->position_reliable)
    {
        bool clock_continuous = stream->capture_clock_valid &&
                                !timing->discontinuity &&
                                timing->clock_id == stream->capture_clock_id &&
                                timing->rate_num == stream->capture_clock_rate_num &&
                                timing->rate_denom == stream->capture_clock_rate_denom &&
                                timing->position >= stream->capture_clock_origin_position;
        if (clock_continuous)
        {
            const uint64_t delta_ticks = timing->position -
                                         stream->capture_clock_origin_position;
            const uint64_t delta_samples = wd_audio_graph_ticks_to_samples(
                delta_ticks, timing->rate_num, timing->rate_denom);
            if (UINT64_MAX - stream->capture_clock_origin_pts < delta_samples)
            {
                clock_continuous = false;
            }
            else
            {
                first_pts = stream->capture_clock_origin_pts + delta_samples;
            }
        }

        if (!clock_continuous)
        {
            if (stream->capture_pts_valid)
            {
                __atomic_store_n(&stream->force_discontinuity, true,
                                 __ATOMIC_RELEASE);
            }
            stream->capture_clock_valid = true;
            stream->capture_clock_origin_position = timing->position;
            stream->capture_clock_origin_pts = observed_first_pts;
            stream->capture_clock_id = timing->clock_id;
            stream->capture_clock_rate_num = timing->rate_num;
            stream->capture_clock_rate_denom = timing->rate_denom;
            first_pts = observed_first_pts;
        }
        else if (stream->capture_pts_valid &&
                 wd_audio_abs_difference(first_pts, stream->capture_next_pts) >
                     tolerance)
        {
            /* Defensive fallback for a driver that changes position without
             * setting DISCONT. Re-anchor to the monotonic graph timestamp. */
            __atomic_store_n(&stream->force_discontinuity, true, __ATOMIC_RELEASE);
            stream->capture_clock_origin_position = timing->position;
            stream->capture_clock_origin_pts = observed_first_pts;
            first_pts = observed_first_pts;
        }
    }
    else
    {
        stream->capture_clock_valid = false;
        if (stream->capture_pts_valid)
        {
            const uint64_t difference = wd_audio_abs_difference(
                observed_first_pts, stream->capture_next_pts);
            if (!timing->discontinuity && difference <= tolerance)
            {
                first_pts = stream->capture_next_pts;
            }
            else
            {
                __atomic_store_n(&stream->force_discontinuity, true,
                                 __ATOMIC_RELEASE);
            }
        }
    }

    stream->capture_pts_valid = true;
    stream->capture_next_pts = UINT64_MAX - first_pts < frames
        ? UINT64_MAX
        : first_pts + frames;

    if (!wd_audio_pcm_ring_write(&stream->ring, samples, frames, first_pts))
    {
        __atomic_store_n(&stream->force_discontinuity, true, __ATOMIC_RELEASE);
        __atomic_add_fetch(&stream->stats.capture_overruns, 1, __ATOMIC_RELAXED);
    }
    else
    {
        __atomic_add_fetch(&stream->stats.captured_frames, frames, __ATOMIC_RELAXED);
    }
    __atomic_sub_fetch(&stream->capture_callbacks_active, 1, __ATOMIC_ACQ_REL);
}

static bool wd_audio_stream_send_config(struct wd_audio_stream* stream) {
    struct wd_audio_config_payload config;
    memset(&config, 0, sizeof(config));
    config.session_id = stream->packetizer.session_id;
    config.connection_token = stream->packetizer.connection_token;
    config.audio_epoch = stream->packetizer.audio_epoch;
    config.media_clock_id = stream->packetizer.media_clock_id;
    config.codec = WD_AUDIO_CODEC_OPUS;
    config.sample_rate = WD_AUDIO_SAMPLE_RATE_DEFAULT;
    config.channels = stream->channels;
    config.frame_samples = WD_AUDIO_FRAME_SAMPLES_DEFAULT;
    config.codec_delay_samples = wd_audio_encoder_delay_samples(stream->encoder);
    config.target_bitrate = stream->bitrate;

    return wd_async_tcp_send_message(stream->tx, stream->tcp_fd,
                                     WD_MSG_AUDIO_CONFIG, &config, sizeof(config));
}

static void wd_audio_stream_drop_stale_queue(struct wd_audio_stream* stream) {
    uint32_t dropped = wd_async_tcp_sender_drop_message_type(stream->tx,
                                                              WD_MSG_AUDIO_PACKET);
    if (dropped != 0)
    {
        __atomic_add_fetch(&stream->stats.queue_drops, dropped, __ATOMIC_RELAXED);
    }
    wd_audio_pcm_ring_drop_all(&stream->ring);
    __atomic_store_n(&stream->force_discontinuity, true, __ATOMIC_RELEASE);
}

static void* wd_audio_stream_worker(void* userdata) {
    struct wd_audio_stream* stream = userdata;
    float* pcm = stream->pcm_buffer;
    uint8_t* packet = stream->packet_buffer;
    uint8_t* payload = stream->payload_buffer;

    while (!__atomic_load_n(&stream->stop_requested, __ATOMIC_ACQUIRE))
    {
        if (!wd_audio_capture_healthy(stream->capture))
        {
            WD_LOG_ERROR("audio capture backend failed; closing audio channel");
            __atomic_store_n(&stream->running, false, __ATOMIC_RELEASE);
            __atomic_store_n(&stream->stop_requested, true, __ATOMIC_RELEASE);
            if (stream->tcp_fd >= 0)
            {
                (void)shutdown(stream->tcp_fd, SHUT_RDWR);
            }
            break;
        }

        const uint64_t captured_frames = __atomic_load_n(
            &stream->stats.captured_frames, __ATOMIC_RELAXED);
        if (captured_frames != 0 && !stream->capture_active_logged)
        {
            stream->capture_active_logged = true;
            WD_LOG_INFO("private audio route active: sink=%s captured_frames=%llu",
                        wd_audio_capture_sink_name(stream->capture),
                        (unsigned long long)captured_frames);
        }
        else if (captured_frames == 0 && !stream->capture_idle_logged &&
                 wd_now_ns() - stream->capture_started_ns >=
                     WD_AUDIO_CAPTURE_IDLE_DIAGNOSTIC_NS)
        {
            stream->capture_idle_logged = true;
            WD_LOG_WARN("private audio sink has received no PCM for 10 seconds: "
                        "sink=%s target=%s; the launched application may be idle "
                        "or its playback stream may not be routed to WayDisplay",
                        wd_audio_capture_sink_name(stream->capture),
                        wd_audio_capture_sink_target(stream->capture));
        }

        wd_async_tcp_sender_reap(stream->tx);

        if (wd_async_tcp_sender_pending_bytes(stream->tx) >=
            stream->tx_max_pending_bytes)
        {
            wd_audio_stream_drop_stale_queue(stream);
            wd_audio_sleep_us(WD_AUDIO_WORKER_SLEEP_US);
            continue;
        }

        uint64_t pts_samples = 0;
        bool continuity = true;
        uint32_t got = wd_audio_pcm_ring_read(&stream->ring, pcm,
                                               WD_AUDIO_FRAME_SAMPLES_DEFAULT,
                                               &pts_samples, &continuity);
        if (got != WD_AUDIO_FRAME_SAMPLES_DEFAULT)
        {
            wd_audio_sleep_us(WD_AUDIO_WORKER_SLEEP_US);
            continue;
        }

        if (!continuity || __atomic_exchange_n(&stream->force_discontinuity,
                                                false, __ATOMIC_ACQ_REL))
        {
            if (!wd_audio_encoder_reset(stream->encoder))
            {
                __atomic_add_fetch(&stream->stats.encode_failures, 1,
                                   __ATOMIC_RELAXED);
                __atomic_store_n(&stream->force_discontinuity, true,
                                 __ATOMIC_RELEASE);
                continue;
            }
            wd_audio_packetizer_mark_discontinuity(&stream->packetizer);
            __atomic_add_fetch(&stream->stats.discontinuities, 1,
                               __ATOMIC_RELAXED);
        }

        uint32_t packet_size = 0;
        if (!wd_audio_encoder_encode(stream->encoder, pcm,
                                     WD_AUDIO_FRAME_SAMPLES_DEFAULT,
                                     packet, WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES,
                                     &packet_size))
        {
            __atomic_add_fetch(&stream->stats.encode_failures, 1,
                               __ATOMIC_RELAXED);
            __atomic_store_n(&stream->force_discontinuity, true, __ATOMIC_RELEASE);
            continue;
        }

        struct wd_audio_packet_payload_header header;
        if (!wd_audio_packetizer_make_packet(&stream->packetizer, pts_samples,
                                              WD_AUDIO_FRAME_SAMPLES_DEFAULT,
                                              packet_size, &header))
        {
            continue;
        }
        memcpy(payload, &header, sizeof(header));
        memcpy(payload + sizeof(header), packet, packet_size);

        if (!wd_async_tcp_send_message(stream->tx, stream->tcp_fd,
                                       WD_MSG_AUDIO_PACKET, payload,
                                       (uint32_t)sizeof(header) + packet_size))
        {
            wd_audio_stream_drop_stale_queue(stream);
            continue;
        }

        __atomic_add_fetch(&stream->stats.encoded_packets, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&stream->stats.encoded_bytes, packet_size,
                           __ATOMIC_RELAXED);
    }

    for (unsigned i = 0; i < 20 && wd_async_tcp_sender_inflight(stream->tx) != 0; ++i)
    {
        wd_async_tcp_sender_reap(stream->tx);
        wd_audio_sleep_us(1000);
    }

    return NULL;
}

static bool wd_audio_stream_ensure_capture_locked(struct wd_audio_stream* stream) {
    if (!stream)
    {
        return false;
    }
    if (wd_audio_capture_healthy(stream->capture))
    {
        return true;
    }
    if (stream->worker_started || stream->capture_started)
    {
        return false;
    }

    if (stream->capture)
    {
        WD_LOG_WARN("recreating unavailable PipeWire private audio sink");
        wd_audio_capture_destroy(stream->capture);
        stream->capture = NULL;
    }
    if (!wd_audio_capture_create(&stream->capture, WD_AUDIO_CHANNELS_MAX,
                                 wd_audio_stream_capture, stream))
    {
        WD_LOG_ERROR("failed to recreate PipeWire private audio sink");
        return false;
    }
    return true;
}

bool wd_audio_stream_create(struct wd_audio_stream** out_stream) {
    if (!out_stream || !wd_audio_capture_available() || !wd_audio_encoder_available())
    {
        return false;
    }
    *out_stream = NULL;
    struct wd_audio_stream* stream = calloc(1, sizeof(*stream));
    if (!stream || pthread_mutex_init(&stream->lock, NULL) != 0)
    {
        free(stream);
        return false;
    }
    stream->tcp_fd = -1;
    if (!wd_audio_stream_ensure_capture_locked(stream))
    {
        pthread_mutex_destroy(&stream->lock);
        free(stream);
        return false;
    }
    *out_stream = stream;
    return true;
}

void wd_audio_stream_destroy(struct wd_audio_stream* stream) {
    if (!stream)
    {
        return;
    }
    wd_audio_stream_stop(stream);
    wd_audio_capture_destroy(stream->capture);
    stream->capture = NULL;
    pthread_mutex_destroy(&stream->lock);
    free(stream);
}

bool wd_audio_stream_available(void) {
    return wd_audio_capture_available() && wd_audio_encoder_available();
}

bool wd_audio_stream_ready(struct wd_audio_stream* stream) {
    if (!stream || !wd_audio_stream_available())
    {
        return false;
    }
    pthread_mutex_lock(&stream->lock);
    const bool ready = (stream->worker_started || stream->capture_started)
        ? wd_audio_capture_healthy(stream->capture)
        : wd_audio_stream_ensure_capture_locked(stream);
    pthread_mutex_unlock(&stream->lock);
    return ready;
}

const char* wd_audio_stream_sink_name(const struct wd_audio_stream* stream) {
    return stream ? wd_audio_capture_sink_name(stream->capture) : NULL;
}

const char* wd_audio_stream_sink_target(const struct wd_audio_stream* stream) {
    return stream ? wd_audio_capture_sink_target(stream->capture) : NULL;
}

const char* wd_audio_stream_capture_backend_name(void) {
    return wd_audio_capture_backend_name();
}

const char* wd_audio_stream_encoder_backend_name(void) {
    return wd_audio_encoder_backend_name();
}

bool wd_audio_stream_start(struct wd_audio_stream* stream, int tcp_fd,
                           uint8_t session_id, uint64_t connection_token,
                           uint64_t audio_epoch, uint64_t media_clock_id,
                           uint64_t media_clock_start_ns, uint8_t channels,
                           uint32_t bitrate, uint16_t target_latency_ms) {
    if (!stream || tcp_fd < 0 || session_id == 0 || connection_token == 0 ||
        audio_epoch == 0 || media_clock_id == 0 || media_clock_start_ns == 0 ||
        channels == 0 || channels > WD_AUDIO_CHANNELS_MAX ||
        !wd_audio_stream_available())
    {
        return false;
    }

    wd_audio_stream_stop(stream);
    pthread_mutex_lock(&stream->lock);
    if (!wd_audio_stream_ensure_capture_locked(stream))
    {
        pthread_mutex_unlock(&stream->lock);
        return false;
    }

    memset(&stream->stats, 0, sizeof(stream->stats));
    stream->tcp_fd = tcp_fd;
    stream->channels = channels;
    stream->bitrate = bitrate ? bitrate : WD_AUDIO_BITRATE_DEFAULT;
    stream->target_latency_ms = target_latency_ms;
    stream->tx_max_pending_bytes = wd_audio_tx_max_pending_bytes(stream->bitrate);
    stream->media_clock_start_ns = media_clock_start_ns;
    stream->capture_next_pts = 0;
    stream->capture_clock_origin_position = 0;
    stream->capture_clock_origin_pts = 0;
    stream->capture_clock_id = 0;
    stream->capture_clock_rate_num = 0;
    stream->capture_clock_rate_denom = 0;
    stream->capture_pts_valid = false;
    stream->capture_clock_valid = false;
    stream->capture_active_logged = false;
    stream->capture_idle_logged = false;
    stream->capture_started_ns = wd_now_ns();
    wd_audio_limit_socket_send_buffer(tcp_fd, stream->tx_max_pending_bytes);
    stream->stop_requested = false;
    stream->force_discontinuity = true;

    const uint32_t capacity_frames =
        (WD_AUDIO_SAMPLE_RATE_DEFAULT * WD_AUDIO_CAPTURE_RING_MS) / 1000u;
    const uint32_t sample_count = WD_AUDIO_FRAME_SAMPLES_DEFAULT * channels;
    bool ok = wd_audio_pcm_ring_init(&stream->ring, capacity_frames, channels) &&
              wd_audio_encoder_create(&stream->encoder, channels, stream->bitrate) &&
              wd_async_tcp_sender_create(&stream->tx, 32);
    if (ok)
    {
        stream->pcm_buffer = calloc(sample_count, sizeof(*stream->pcm_buffer));
        stream->packet_buffer = malloc(WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES);
        stream->payload_buffer = malloc(sizeof(struct wd_audio_packet_payload_header) +
                                        WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES);
        ok = stream->pcm_buffer && stream->packet_buffer && stream->payload_buffer;
    }
    if (stream->tx)
    {
        wd_async_tcp_sender_set_max_pending_bytes(stream->tx,
                                                  stream->tx_max_pending_bytes);
    }
    if (ok)
    {
        wd_audio_packetizer_begin(&stream->packetizer, session_id,
                                  connection_token, audio_epoch, media_clock_id);
        ok = wd_audio_stream_send_config(stream);
    }
    if (ok)
    {
        ok = pthread_create(&stream->worker, NULL, wd_audio_stream_worker,
                            stream) == 0;
        stream->worker_started = ok;
    }
    if (ok)
    {
        __atomic_store_n(&stream->capture_delivery_enabled, true, __ATOMIC_RELEASE);
        ok = wd_audio_capture_start(stream->capture);
        stream->capture_started = ok;
        if (!ok)
        {
            __atomic_store_n(&stream->capture_delivery_enabled, false, __ATOMIC_RELEASE);
        }
    }
    if (ok)
    {
        __atomic_store_n(&stream->running, true, __ATOMIC_RELEASE);
        WD_LOG_INFO("audio stream started: capture=%s encoder=%s codec=opus rate=%u channels=%u frame_samples=%u bitrate=%u target_latency_ms=%u",
                    wd_audio_capture_backend_name(), wd_audio_encoder_backend_name(),
                    WD_AUDIO_SAMPLE_RATE_DEFAULT, channels, WD_AUDIO_FRAME_SAMPLES_DEFAULT,
                    stream->bitrate, target_latency_ms);
        WD_LOG_DEBUG("audio transport queue: userspace_max=%llu bytes target_ms=%u",
                     (unsigned long long)stream->tx_max_pending_bytes,
                     WD_AUDIO_TX_QUEUE_MS);
        pthread_mutex_unlock(&stream->lock);
        return true;
    }

    __atomic_store_n(&stream->stop_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&stream->capture_delivery_enabled, false, __ATOMIC_RELEASE);
    if (stream->capture_started)
    {
        wd_audio_capture_stop(stream->capture);
        stream->capture_started = false;
    }
    while (__atomic_load_n(&stream->capture_callbacks_active, __ATOMIC_ACQUIRE) != 0)
    {
        wd_audio_sleep_us(1000);
    }
    if (stream->worker_started)
    {
        pthread_join(stream->worker, NULL);
        stream->worker_started = false;
    }
    wd_async_tcp_sender_destroy(stream->tx);
    wd_audio_encoder_destroy(stream->encoder);
    wd_audio_pcm_ring_finish(&stream->ring);
    free(stream->payload_buffer);
    free(stream->packet_buffer);
    free(stream->pcm_buffer);
    stream->payload_buffer = NULL;
    stream->packet_buffer = NULL;
    stream->pcm_buffer = NULL;
    stream->tx = NULL;
    stream->encoder = NULL;
    stream->tcp_fd = -1;
    pthread_mutex_unlock(&stream->lock);
    return false;
}

void wd_audio_stream_stop(struct wd_audio_stream* stream) {
    if (!stream)
    {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    __atomic_store_n(&stream->stop_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&stream->capture_delivery_enabled, false, __ATOMIC_RELEASE);
    if (stream->capture_started)
    {
        wd_audio_capture_stop(stream->capture);
        stream->capture_started = false;
    }
    while (__atomic_load_n(&stream->capture_callbacks_active, __ATOMIC_ACQUIRE) != 0)
    {
        wd_audio_sleep_us(1000);
    }
    if (stream->worker_started)
    {
        pthread_join(stream->worker, NULL);
        stream->worker_started = false;
    }
    wd_async_tcp_sender_destroy(stream->tx);
    wd_audio_encoder_destroy(stream->encoder);
    wd_audio_pcm_ring_finish(&stream->ring);
    free(stream->payload_buffer);
    free(stream->packet_buffer);
    free(stream->pcm_buffer);
    stream->payload_buffer = NULL;
    stream->packet_buffer = NULL;
    stream->pcm_buffer = NULL;
    stream->tx = NULL;
    stream->encoder = NULL;
    stream->tcp_fd = -1;
    __atomic_store_n(&stream->running, false, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&stream->lock);
}

bool wd_audio_stream_running(const struct wd_audio_stream* stream) {
    return stream && __atomic_load_n(&stream->running, __ATOMIC_ACQUIRE);
}

void wd_audio_stream_get_stats(const struct wd_audio_stream* stream,
                               struct wd_audio_stream_stats* stats) {
    if (!stats)
    {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!stream)
    {
        return;
    }
    stats->captured_frames = __atomic_load_n(&stream->stats.captured_frames, __ATOMIC_RELAXED);
    stats->capture_overruns = __atomic_load_n(&stream->stats.capture_overruns, __ATOMIC_RELAXED);
    stats->encoded_packets = __atomic_load_n(&stream->stats.encoded_packets, __ATOMIC_RELAXED);
    stats->encoded_bytes = __atomic_load_n(&stream->stats.encoded_bytes, __ATOMIC_RELAXED);
    stats->queue_drops = __atomic_load_n(&stream->stats.queue_drops, __ATOMIC_RELAXED);
    stats->discontinuities = __atomic_load_n(&stream->stats.discontinuities, __ATOMIC_RELAXED);
    stats->encode_failures = __atomic_load_n(&stream->stats.encode_failures, __ATOMIC_RELAXED);
}
