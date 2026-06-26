#include "client_session.h"

#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                                                                  \
    do                                                                                                                                      \
    {                                                                                                                                       \
        if (!(condition))                                                                                                                   \
        {                                                                                                                                   \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                         \
            return 1;                                                                                                                       \
        }                                                                                                                                   \
    } while (0)

int main(void) {
    struct wd_client_session session = WD_CLIENT_SESSION_INITIALIZER;
    REQUIRE(wd_client_session_fds_closed(&session));
    REQUIRE(wd_client_session_begin_connect(&session));
    REQUIRE(!wd_client_session_begin_connect(&session));

    int pair[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    session.control_fd = pair[0];
    wd_client_session_mark_connected(&session);
    REQUIRE(session.phase == WD_CLIENT_SESSION_CONNECTED);

    REQUIRE(wd_client_session_begin_shutdown(&session));
    REQUIRE(!wd_client_session_begin_shutdown(&session));
    wd_client_session_shutdown_open_fds(&session);

    char byte = 0;
    REQUIRE(recv(pair[1], &byte, sizeof(byte), 0) == 0);
    close(pair[1]);

    wd_client_session_close_open_fds(&session);
    REQUIRE(session.phase == WD_CLIENT_SESSION_IDLE);
    REQUIRE(wd_client_session_fds_closed(&session));
    wd_client_session_close_open_fds(&session);
    REQUIRE(wd_client_session_fds_closed(&session));
    return 0;
}
