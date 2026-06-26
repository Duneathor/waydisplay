#include "waydisplay/wd_eventfd.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr);                                         \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

struct signal_context {
    int      fd;
    uint32_t count;
    bool     ok;
};

static void* signal_main(void* data) {
    struct signal_context* context = data;
    context->ok = true;
    for (uint32_t i = 0; i < context->count; ++i)
    {
        if (!wd_eventfd_signal(context->fd))
        {
            context->ok = false;
            break;
        }
    }
    return NULL;
}

int main(void) {
    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(fd >= 0);

    pthread_mutex_t held_lock;
    CHECK(pthread_mutex_init(&held_lock, NULL) == 0);
    CHECK(pthread_mutex_lock(&held_lock) == 0);
    CHECK(wd_eventfd_signal(fd));
    CHECK(pthread_mutex_unlock(&held_lock) == 0);

    enum { THREADS = 4, SIGNALS_PER_THREAD = 1000 };
    pthread_t threads[THREADS];
    struct signal_context contexts[THREADS];
    for (uint32_t i = 0; i < THREADS; ++i)
    {
        contexts[i].fd    = fd;
        contexts[i].count = SIGNALS_PER_THREAD;
        contexts[i].ok    = false;
        CHECK(pthread_create(&threads[i], NULL, signal_main, &contexts[i]) == 0);
    }
    for (uint32_t i = 0; i < THREADS; ++i)
    {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(contexts[i].ok);
    }

    uint64_t value = 0;
    CHECK(read(fd, &value, sizeof(value)) == (ssize_t)sizeof(value));
    CHECK(value == 1u + (uint64_t)THREADS * SIGNALS_PER_THREAD);

    pthread_mutex_destroy(&held_lock);
    close(fd);
    return 0;
}
