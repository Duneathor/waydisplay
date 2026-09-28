#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_socket_pin {
    int      source_fd;
    int      io_fd;
    uint64_t cookie;
};

#define WD_SOCKET_PIN_INITIALIZER {-1, -1, 0}

static inline bool wd_socket_cookie(int fd, uint64_t* out_cookie) {
    if (fd < 0 || !out_cookie)
    {
        return false;
    }
    uint64_t cookie = 0;
    socklen_t size = sizeof(cookie);
    if (getsockopt(fd, SOL_SOCKET, SO_COOKIE, &cookie, &size) != 0 || size != sizeof(cookie) || cookie == 0)
    {
        return false;
    }
    *out_cookie = cookie;
    return true;
}

static inline void wd_socket_pin_reset(struct wd_socket_pin* pin) {
    if (!pin)
    {
        return;
    }
    if (pin->io_fd >= 0)
    {
        close(pin->io_fd);
    }
    pin->source_fd = -1;
    pin->io_fd = -1;
    pin->cookie = 0;
}

static inline bool wd_socket_pin_matches(const struct wd_socket_pin* pin, int fd) {
    if (!pin || pin->io_fd < 0 || pin->cookie == 0 || fd < 0)
    {
        return false;
    }
    uint64_t cookie = 0;
    return wd_socket_cookie(fd, &cookie) && cookie == pin->cookie;
}

static inline bool wd_socket_pin_bind(struct wd_socket_pin* pin, int fd) {
    if (!pin || fd < 0)
    {
        return false;
    }
    uint64_t cookie = 0;
    if (!wd_socket_cookie(fd, &cookie))
    {
        return false;
    }
    if (pin->io_fd >= 0 && pin->cookie == cookie)
    {
        pin->source_fd = fd;
        return true;
    }

    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0)
    {
        return false;
    }
    uint64_t duplicate_cookie = 0;
    if (!wd_socket_cookie(duplicate, &duplicate_cookie) || duplicate_cookie != cookie)
    {
        close(duplicate);
        return false;
    }

    wd_socket_pin_reset(pin);
    pin->source_fd = fd;
    pin->io_fd = duplicate;
    pin->cookie = cookie;
    return true;
}

static inline bool wd_socket_pin_shutdown(struct wd_socket_pin* pin) {
    if (!pin || pin->io_fd < 0)
    {
        return false;
    }
    (void)shutdown(pin->io_fd, SHUT_RDWR);
    return true;
}

#ifdef __cplusplus
}
#endif
