#include "wd_stream_pipeline_internal.h"

#include "wd_server_compositor.h"
#include "wd_server_net.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct wd_stream_frame_worker {
    struct wd_server* server;
    pthread_t         thread;
    pthread_mutex_t   lock;
    pthread_cond_t    cond;
    bool              thread_started;
    bool              running;
    bool              active;
    bool              frame_pending;
    bool              service_pending;
    bool*             damage_tiles;
    uint32_t          damage_capacity;
    bool              damage_all_tiles;
    uint32_t          damage_tile_count;
};

static bool worker_resize_damage_snapshot(struct wd_stream_frame_worker* worker, uint32_t tile_count) {
    if (!worker)
    {
        return false;
    }
    if (tile_count <= worker->damage_capacity)
    {
        return true;
    }

    bool* resized = realloc(worker->damage_tiles, (size_t)tile_count * sizeof(*resized));
    if (!resized)
    {
        return false;
    }
    worker->damage_tiles    = resized;
    worker->damage_capacity = tile_count;
    return true;
}

static void* stream_frame_worker_main(void* data) {
    struct wd_stream_frame_worker* worker = data;
    if (!worker)
    {
        return NULL;
    }

    for (;;)
    {
        pthread_mutex_lock(&worker->lock);
        while (worker->running && !worker->frame_pending && !worker->service_pending)
        {
            pthread_cond_wait(&worker->cond, &worker->lock);
        }
        if (!worker->running)
        {
            pthread_mutex_unlock(&worker->lock);
            break;
        }

        const bool process_frame = worker->frame_pending;
        worker->frame_pending    = false;
        worker->service_pending  = false;
        worker->active           = true;
        struct wd_stream_damage_view damage = {
            .tiles      = worker->damage_tiles,
            .all_tiles  = worker->damage_all_tiles,
            .tile_count = worker->damage_tile_count,
        };
        pthread_mutex_unlock(&worker->lock);

        if (process_frame)
        {
            (void)wd_stream_process_frame(worker->server, &damage);
        }
        else
        {
            (void)wd_stream_process_queued_work(worker->server);
        }

        pthread_mutex_lock(&worker->lock);
        worker->active = false;
        pthread_cond_broadcast(&worker->cond);
        pthread_mutex_unlock(&worker->lock);

        /* New compositor damage may have accumulated while stream processing
         * was active. Wake the event loop; the compositor callback pulls the
         * next frame-service deadline forward when work is pending. */
        wd_server_wake_input(worker->server);
    }

    return NULL;
}

bool wd_stream_frame_worker_init(struct wd_server* server) {
    if (!server)
    {
        return false;
    }
    if (server->stream_frame_worker)
    {
        return true;
    }

    struct wd_stream_frame_worker* worker = calloc(1, sizeof(*worker));
    if (!worker)
    {
        return false;
    }
    worker->server = server;
    worker->running = true;

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
    if (!worker_resize_damage_snapshot(worker, server->total_base_tiles) ||
        pthread_create(&worker->thread, NULL, stream_frame_worker_main, worker) != 0)
    {
        free(worker->damage_tiles);
        pthread_cond_destroy(&worker->cond);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return false;
    }

    worker->thread_started       = true;
    server->stream_frame_worker  = worker;
    return true;
}

void wd_stream_frame_worker_destroy(struct wd_server* server) {
    if (!server || !server->stream_frame_worker)
    {
        return;
    }

    struct wd_stream_frame_worker* worker = server->stream_frame_worker;
    server->stream_frame_worker           = NULL;

    pthread_mutex_lock(&worker->lock);
    worker->running         = false;
    worker->frame_pending   = false;
    worker->service_pending = false;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    if (worker->thread_started)
    {
        pthread_join(worker->thread, NULL);
    }

    free(worker->damage_tiles);
    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->lock);
    free(worker);
}

bool wd_stream_frame_worker_idle(struct wd_server* server) {
    if (!server || !server->stream_frame_worker)
    {
        return true;
    }

    struct wd_stream_frame_worker* worker = server->stream_frame_worker;
    pthread_mutex_lock(&worker->lock);
    const bool idle = !worker->active && !worker->frame_pending && !worker->service_pending;
    pthread_mutex_unlock(&worker->lock);
    return idle;
}

bool wd_stream_frame_worker_submit(struct wd_server* server) {
    if (!server || !server->stream_frame_worker)
    {
        return false;
    }

    struct wd_stream_frame_worker* worker = server->stream_frame_worker;
    pthread_mutex_lock(&worker->lock);
    if (!worker->running || worker->active || worker->frame_pending ||
        !worker_resize_damage_snapshot(worker, server->total_base_tiles))
    {
        pthread_mutex_unlock(&worker->lock);
        return false;
    }

    if (server->damage_tiles && server->total_base_tiles != 0)
    {
        memcpy(worker->damage_tiles, server->damage_tiles,
               (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }
    worker->damage_all_tiles  = server->damage_all_tiles;
    worker->damage_tile_count = server->damage_tile_count;
    worker->frame_pending     = true;
    worker->service_pending   = false;

    wd_server_clear_scene_damage(server);
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
    return true;
}

void wd_stream_frame_worker_request_service(struct wd_server* server) {
    if (!server || !server->stream_frame_worker)
    {
        return;
    }

    struct wd_stream_frame_worker* worker = server->stream_frame_worker;
    pthread_mutex_lock(&worker->lock);
    if (worker->running && !worker->frame_pending)
    {
        worker->service_pending = true;
        pthread_cond_signal(&worker->cond);
    }
    pthread_mutex_unlock(&worker->lock);
}
