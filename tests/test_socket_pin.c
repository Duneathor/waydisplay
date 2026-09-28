#include "waydisplay/wd_socket_pin.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int old_pair[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, old_pair) == 0);

    struct wd_socket_pin pin = WD_SOCKET_PIN_INITIALIZER;
    assert(wd_socket_pin_bind(&pin, old_pair[0]));
    const int recycled_fd = old_pair[0];
    close(old_pair[0]);
    old_pair[0] = -1;

    int new_pair[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, new_pair) == 0);
    if (new_pair[0] != recycled_fd)
    {
        if (new_pair[1] == recycled_fd)
        {
            const int moved_peer = dup(new_pair[1]);
            assert(moved_peer >= 0);
            assert(fcntl(moved_peer, F_SETFD, FD_CLOEXEC) == 0);
            close(new_pair[1]);
            new_pair[1] = moved_peer;
        }

        assert(dup2(new_pair[0], recycled_fd) == recycled_fd);
        assert(fcntl(recycled_fd, F_SETFD, FD_CLOEXEC) == 0);
        close(new_pair[0]);
        new_pair[0] = recycled_fd;
    }

    assert(!wd_socket_pin_matches(&pin, recycled_fd));
    assert(wd_socket_pin_shutdown(&pin));

    const uint8_t sent = 0x5a;
    uint8_t received = 0;
    assert(send(new_pair[0], &sent, 1, MSG_NOSIGNAL) == 1);
    assert(recv(new_pair[1], &received, 1, MSG_WAITALL) == 1);
    assert(received == sent);

    wd_socket_pin_reset(&pin);
    close(old_pair[1]);
    close(new_pair[0]);
    close(new_pair[1]);
    return 0;
}
