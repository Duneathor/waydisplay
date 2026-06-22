#define _POSIX_C_SOURCE 200809L

#include "wd_process.h"

#include "waydisplay/wd_config.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

extern char** environ;

static bool wd_process_env_name_valid(const char* name) {
    return name && name[0] != '\0' && strchr(name, '=') == NULL;
}

static bool wd_process_env_entry_matches(const char* entry, const char* name) {
    const size_t name_length = strlen(name);
    return strncmp(entry, name, name_length) == 0 && entry[name_length] == '=';
}

static const struct wd_process_env_change* wd_process_find_change(
    const struct wd_process_env_change* changes, size_t change_count, const char* entry) {
    for (size_t i = 0; i < change_count; ++i)
    {
        if (wd_process_env_entry_matches(entry, changes[i].name))
        {
            return &changes[i];
        }
    }
    return NULL;
}

static char* wd_process_make_env_entry(const char* name, const char* value) {
    const size_t name_length  = strlen(name);
    const size_t value_length = strlen(value);
    if (name_length > SIZE_MAX - value_length - 2u)
    {
        errno = EOVERFLOW;
        return NULL;
    }

    char* entry = malloc(name_length + value_length + 2u);
    if (!entry)
    {
        return NULL;
    }

    memcpy(entry, name, name_length);
    entry[name_length] = '=';
    memcpy(entry + name_length + 1u, value, value_length + 1u);
    return entry;
}

static void wd_process_free_environment(char** environment, size_t inherited_count) {
    if (!environment)
    {
        return;
    }

    for (size_t i = inherited_count; environment[i] != NULL; ++i)
    {
        free(environment[i]);
    }
    free(environment);
}

static char** wd_process_build_environment(const struct wd_process_env_change* changes,
                                           size_t change_count, size_t* inherited_count_out,
                                           int* error_code) {
    for (size_t i = 0; i < change_count; ++i)
    {
        if (!wd_process_env_name_valid(changes[i].name) ||
            (changes[i].action != WD_PROCESS_ENV_UNSET && !changes[i].value))
        {
            if (error_code)
            {
                *error_code = EINVAL;
            }
            return NULL;
        }
    }

    size_t inherited_capacity = 0;
    while (environ[inherited_capacity] != NULL)
    {
        ++inherited_capacity;
    }

    if (inherited_capacity > SIZE_MAX - change_count - 1u)
    {
        if (error_code)
        {
            *error_code = EOVERFLOW;
        }
        return NULL;
    }

    char** environment = calloc(inherited_capacity + change_count + 1u, sizeof(*environment));
    bool*  change_seen = change_count != 0 ? calloc(change_count, sizeof(*change_seen)) : NULL;
    if (!environment || (change_count != 0 && !change_seen))
    {
        free(environment);
        free(change_seen);
        if (error_code)
        {
            *error_code = ENOMEM;
        }
        return NULL;
    }

    size_t environment_count = 0;
    for (size_t i = 0; i < inherited_capacity; ++i)
    {
        const struct wd_process_env_change* change =
            wd_process_find_change(changes, change_count, environ[i]);
        if (!change)
        {
            environment[environment_count++] = environ[i];
            continue;
        }

        const size_t change_index = (size_t)(change - changes);
        change_seen[change_index] = true;
        if (change->action == WD_PROCESS_ENV_SET_IF_ABSENT)
        {
            environment[environment_count++] = environ[i];
        }
    }

    const size_t inherited_count = environment_count;
    for (size_t i = 0; i < change_count; ++i)
    {
        if (changes[i].action == WD_PROCESS_ENV_UNSET ||
            (changes[i].action == WD_PROCESS_ENV_SET_IF_ABSENT && change_seen[i]))
        {
            continue;
        }

        char* entry = wd_process_make_env_entry(changes[i].name, changes[i].value);
        if (!entry)
        {
            const int saved_errno = errno != 0 ? errno : ENOMEM;
            free(change_seen);
            wd_process_free_environment(environment, inherited_count);
            if (error_code)
            {
                *error_code = saved_errno;
            }
            return NULL;
        }
        environment[environment_count++] = entry;
    }
    environment[environment_count] = NULL;

    free(change_seen);
    if (inherited_count_out)
    {
        *inherited_count_out = inherited_count;
    }
    return environment;
}

void wd_spawned_process_init(struct wd_spawned_process* process) {
    if (!process)
    {
        return;
    }
    process->pid           = -1;
    process->process_group = -1;
}

bool wd_spawn_shell_command(struct wd_spawned_process* process, const char* command,
                            const struct wd_process_env_change* changes, size_t change_count,
                            int* error_code) {
    if (error_code)
    {
        *error_code = 0;
    }
    if (!process || !command || command[0] == '\0' || (change_count != 0 && !changes))
    {
        if (error_code)
        {
            *error_code = EINVAL;
        }
        return false;
    }

    wd_spawned_process_init(process);

    size_t inherited_count = 0;
    int environment_error = 0;
    char** child_environment = wd_process_build_environment(
        changes, change_count, &inherited_count, &environment_error);
    if (!child_environment)
    {
        if (error_code)
        {
            *error_code = environment_error;
        }
        return false;
    }

    posix_spawnattr_t attributes;
    int spawn_error = posix_spawnattr_init(&attributes);
    if (spawn_error != 0)
    {
        wd_process_free_environment(child_environment, inherited_count);
        if (error_code)
        {
            *error_code = spawn_error;
        }
        return false;
    }

    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    sigset_t default_signals;
    sigset_t signal_mask;
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGINT);
    sigaddset(&default_signals, SIGTERM);
    sigaddset(&default_signals, SIGPIPE);
    sigemptyset(&signal_mask);

    spawn_error = posix_spawnattr_setflags(&attributes, flags);
    if (spawn_error == 0)
    {
        spawn_error = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (spawn_error == 0)
    {
        spawn_error = posix_spawnattr_setsigdefault(&attributes, &default_signals);
    }
    if (spawn_error == 0)
    {
        spawn_error = posix_spawnattr_setsigmask(&attributes, &signal_mask);
    }

    pid_t child_pid = -1;
    if (spawn_error == 0)
    {
        char* const argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)command, NULL};
        spawn_error = posix_spawn(&child_pid, "/bin/sh", NULL, &attributes, argv,
                                  child_environment);
    }

    posix_spawnattr_destroy(&attributes);
    wd_process_free_environment(child_environment, inherited_count);

    if (spawn_error != 0)
    {
        if (error_code)
        {
            *error_code = spawn_error;
        }
        return false;
    }

    process->pid           = child_pid;
    process->process_group = child_pid;
    return true;
}

static void wd_process_sleep_milliseconds(unsigned int milliseconds) {
    struct timespec delay = {
        .tv_sec  = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
    };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
    {
    }
}

enum wd_process_reap_result wd_spawned_process_reap_nonblocking(
    struct wd_spawned_process* process, int* status, int* error_code) {
    if (status)
    {
        *status = 0;
    }
    if (error_code)
    {
        *error_code = 0;
    }
    if (!process || process->pid <= 0)
    {
        return WD_PROCESS_REAP_NONE;
    }

    int child_status = 0;
    pid_t result;
    do
    {
        result = waitpid(process->pid, &child_status, WNOHANG);
    } while (result < 0 && errno == EINTR);

    if (result == 0)
    {
        return WD_PROCESS_REAP_RUNNING;
    }
    if (result == process->pid)
    {
        process->pid = -1;
        if (status)
        {
            *status = child_status;
        }
        return WD_PROCESS_REAP_EXITED;
    }

    const int saved_errno = errno;
    if (saved_errno == ECHILD)
    {
        process->pid = -1;
    }
    if (error_code)
    {
        *error_code = saved_errno;
    }
    return WD_PROCESS_REAP_ERROR;
}

bool wd_spawned_process_group_alive(const struct wd_spawned_process* process) {
    if (!process || process->process_group <= 0)
    {
        return false;
    }

    if (kill(-process->process_group, 0) == 0)
    {
        return true;
    }
    return errno == EPERM;
}

static bool wd_spawned_process_wait_for_group(struct wd_spawned_process* process,
                                              unsigned int timeout_ms, int* status,
                                              int* error_code) {
    unsigned int elapsed_ms = 0;
    for (;;)
    {
        int child_status = 0;
        int reap_error = 0;
        const enum wd_process_reap_result reap_result =
            wd_spawned_process_reap_nonblocking(process, &child_status, &reap_error);
        if (reap_result == WD_PROCESS_REAP_EXITED && status)
        {
            *status = child_status;
        }
        else if (reap_result == WD_PROCESS_REAP_ERROR && reap_error != ECHILD)
        {
            if (error_code)
            {
                *error_code = reap_error;
            }
            return false;
        }

        if (!wd_spawned_process_group_alive(process))
        {
            process->process_group = -1;
            return true;
        }

        if (elapsed_ms >= timeout_ms)
        {
            return false;
        }
        const unsigned int sleep_ms = timeout_ms - elapsed_ms < WD_SERVER_PROCESS_POLL_INTERVAL_MS
                                          ? timeout_ms - elapsed_ms
                                          : WD_SERVER_PROCESS_POLL_INTERVAL_MS;
        wd_process_sleep_milliseconds(sleep_ms);
        elapsed_ms += sleep_ms;
    }
}

bool wd_spawned_process_terminate_group(struct wd_spawned_process* process,
                                        unsigned int terminate_timeout_ms,
                                        unsigned int kill_timeout_ms, int* status,
                                        int* error_code) {
    if (status)
    {
        *status = 0;
    }
    if (error_code)
    {
        *error_code = 0;
    }
    if (!process)
    {
        if (error_code)
        {
            *error_code = EINVAL;
        }
        return false;
    }

    if (!wd_spawned_process_group_alive(process))
    {
        int child_status = 0;
        int reap_error = 0;
        const enum wd_process_reap_result reap_result =
            wd_spawned_process_reap_nonblocking(process, &child_status, &reap_error);
        if (reap_result == WD_PROCESS_REAP_EXITED && status)
        {
            *status = child_status;
        }
        else if (reap_result == WD_PROCESS_REAP_ERROR && reap_error != ECHILD)
        {
            if (error_code)
            {
                *error_code = reap_error;
            }
            return false;
        }
        process->process_group = -1;
        return true;
    }

    if (kill(-process->process_group, SIGTERM) < 0 && errno != ESRCH)
    {
        if (error_code)
        {
            *error_code = errno;
        }
        return false;
    }

    if (wd_spawned_process_wait_for_group(process, terminate_timeout_ms, status, error_code))
    {
        return true;
    }

    if (kill(-process->process_group, SIGKILL) < 0 && errno != ESRCH)
    {
        if (error_code)
        {
            *error_code = errno;
        }
        return false;
    }

    if (wd_spawned_process_wait_for_group(process, kill_timeout_ms, status, error_code))
    {
        return true;
    }

    if (error_code && *error_code == 0)
    {
        *error_code = ETIMEDOUT;
    }
    return false;
}
