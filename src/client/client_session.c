#include "client_session.h"

#include <sys/socket.h>
#include <unistd.h>

static void wd_client_session_shutdown_fd(int fd) {
    if (fd >= 0)
    {
        (void)shutdown(fd, SHUT_RDWR);
    }
}

static void wd_client_session_close_fd(int* fd) {
    if (fd && *fd >= 0)
    {
        (void)close(*fd);
        *fd = -1;
    }
}

void wd_client_session_init(struct wd_client_session* session) {
    if (!session)
    {
        return;
    }
    *session = (struct wd_client_session)WD_CLIENT_SESSION_INITIALIZER;
}

bool wd_client_session_begin_connect(struct wd_client_session* session) {
    if (!session || session->phase != WD_CLIENT_SESSION_IDLE || !wd_client_session_fds_closed(session))
    {
        return false;
    }
    session->phase = WD_CLIENT_SESSION_CONNECTING;
    return true;
}

void wd_client_session_mark_connected(struct wd_client_session* session) {
    if (session && session->phase == WD_CLIENT_SESSION_CONNECTING)
    {
        session->phase = WD_CLIENT_SESSION_CONNECTED;
    }
}

bool wd_client_session_begin_shutdown(struct wd_client_session* session) {
    if (!session || session->phase == WD_CLIENT_SESSION_STOPPING)
    {
        return false;
    }
    session->phase = WD_CLIENT_SESSION_STOPPING;
    return true;
}

void wd_client_session_shutdown_open_fds(const struct wd_client_session* session) {
    if (!session)
    {
        return;
    }
    wd_client_session_shutdown_fd(session->control_fd);
    wd_client_session_shutdown_fd(session->input_fd);
    wd_client_session_shutdown_fd(session->selection_fd);
    wd_client_session_shutdown_fd(session->video_fd);
    wd_client_session_shutdown_fd(session->audio_fd);
    wd_client_session_shutdown_fd(session->udp_fd);
}

void wd_client_session_close_open_fds(struct wd_client_session* session) {
    if (!session)
    {
        return;
    }
    wd_client_session_close_fd(&session->control_fd);
    wd_client_session_close_fd(&session->input_fd);
    wd_client_session_close_fd(&session->selection_fd);
    wd_client_session_close_fd(&session->video_fd);
    wd_client_session_close_fd(&session->audio_fd);
    wd_client_session_close_fd(&session->udp_fd);
    session->phase = WD_CLIENT_SESSION_IDLE;
}

bool wd_client_session_fds_closed(const struct wd_client_session* session) {
    return session && session->control_fd < 0 && session->input_fd < 0 && session->selection_fd < 0 && session->video_fd < 0 &&
           session->audio_fd < 0 && session->udp_fd < 0;
}
