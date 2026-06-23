#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_process_env_action {
    WD_PROCESS_ENV_SET = 0,
    WD_PROCESS_ENV_UNSET,
    WD_PROCESS_ENV_SET_IF_ABSENT,
};

struct wd_process_env_change {
    const char*                name;
    const char*                value;
    enum wd_process_env_action action;
};

struct wd_spawned_process {
    pid_t pid;
    pid_t process_group;
};

enum wd_process_reap_result {
    WD_PROCESS_REAP_NONE = 0,
    WD_PROCESS_REAP_RUNNING,
    WD_PROCESS_REAP_EXITED,
    WD_PROCESS_REAP_ERROR,
};

void wd_spawned_process_init(struct wd_spawned_process* process);
bool wd_spawn_shell_command(struct wd_spawned_process* process, const char* command, const struct wd_process_env_change* changes,
                            size_t change_count, int* error_code);
enum wd_process_reap_result wd_spawned_process_reap_nonblocking(struct wd_spawned_process* process, int* status, int* error_code);
bool                        wd_spawned_process_group_alive(const struct wd_spawned_process* process);
bool wd_spawned_process_terminate_group(struct wd_spawned_process* process, unsigned int terminate_timeout_ms, unsigned int kill_timeout_ms,
                                        int* status, int* error_code);

#ifdef __cplusplus
}
#endif
