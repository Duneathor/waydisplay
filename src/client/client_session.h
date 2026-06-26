#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_session_phase {
    WD_CLIENT_SESSION_IDLE = 0,
    WD_CLIENT_SESSION_CONNECTING,
    WD_CLIENT_SESSION_CONNECTED,
    WD_CLIENT_SESSION_STOPPING,
};

struct wd_client_session {
    int control_fd;
    int input_fd;
    int selection_fd;
    int video_fd;
    int audio_fd;
    int udp_fd;
    enum wd_client_session_phase phase;
};

#define WD_CLIENT_SESSION_INITIALIZER {-1, -1, -1, -1, -1, -1, WD_CLIENT_SESSION_IDLE}

void wd_client_session_init(struct wd_client_session* session);
bool wd_client_session_begin_connect(struct wd_client_session* session);
void wd_client_session_mark_connected(struct wd_client_session* session);
bool wd_client_session_begin_shutdown(struct wd_client_session* session);
void wd_client_session_shutdown_open_fds(const struct wd_client_session* session);
void wd_client_session_close_open_fds(struct wd_client_session* session);
bool wd_client_session_fds_closed(const struct wd_client_session* session);

#ifdef __cplusplus
}
#endif
