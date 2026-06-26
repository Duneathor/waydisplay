#include "waydisplay/wd_eventfd.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

bool wd_eventfd_signal(int fd) {
    if (fd < 0)
    {
        return false;
    }

    const uint64_t value = 1;
    ssize_t        written;
    do
    {
        written = write(fd, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);

    return written == (ssize_t)sizeof(value);
}
