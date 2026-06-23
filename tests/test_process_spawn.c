#include "wd_process.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            exit(1);                                                                                                                       \
        }                                                                                                                                  \
    } while (0)

static void sleep_milliseconds(unsigned int milliseconds) {
    struct timespec delay = {
        .tv_sec  = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
    };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
    {
    }
}

static int reap_with_timeout(struct wd_spawned_process* process) {
    for (unsigned int attempt = 0; attempt < 200u; ++attempt)
    {
        int                         status     = 0;
        int                         error_code = 0;
        enum wd_process_reap_result result     = wd_spawned_process_reap_nonblocking(process, &status, &error_code);
        CHECK(result != WD_PROCESS_REAP_ERROR);
        CHECK(error_code == 0);
        if (result == WD_PROCESS_REAP_EXITED)
        {
            return status;
        }
        sleep_milliseconds(10u);
    }

    CHECK(!"timed out waiting for child exit");
    return 0;
}

int main(void) {
    CHECK(setenv("WD_TEST_SET", "old", 1) == 0);
    CHECK(setenv("WD_TEST_UNSET", "remove", 1) == 0);
    CHECK(setenv("WD_TEST_KEEP", "original", 1) == 0);

    const struct wd_process_env_change changes[] = {
        {.name = "WD_TEST_SET", .value = "new", .action = WD_PROCESS_ENV_SET},
        {.name = "WD_TEST_UNSET", .value = NULL, .action = WD_PROCESS_ENV_UNSET},
        {.name = "WD_TEST_KEEP", .value = "replacement", .action = WD_PROCESS_ENV_SET_IF_ABSENT},
        {.name = "WD_TEST_ADD", .value = "added", .action = WD_PROCESS_ENV_SET_IF_ABSENT},
    };

    struct wd_spawned_process process;
    int                       error_code = 0;
    CHECK(wd_spawn_shell_command(&process,
                                 "test \"$WD_TEST_SET\" = new && "
                                 "test -z \"${WD_TEST_UNSET+x}\" && "
                                 "test \"$WD_TEST_KEEP\" = original && "
                                 "test \"$WD_TEST_ADD\" = added && sleep 0.1",
                                 changes, sizeof(changes) / sizeof(changes[0]), &error_code));
    CHECK(process.pid > 0);
    CHECK(process.process_group == process.pid);
    CHECK(getpgid(process.pid) == process.process_group);

    int status = reap_with_timeout(&process);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(process.pid == -1);
    CHECK(wd_spawned_process_terminate_group(&process, 100u, 100u, NULL, &error_code));
    CHECK(process.process_group == -1);

    struct wd_spawned_process unchanged;
    error_code = 0;
    CHECK(wd_spawn_shell_command(&unchanged, "true", NULL, 0, &error_code));
    status = reap_with_timeout(&unchanged);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(wd_spawned_process_terminate_group(&unchanged, 100u, 100u, NULL, &error_code));

    struct wd_spawned_process descendants;
    error_code = 0;
    CHECK(wd_spawn_shell_command(&descendants, "trap '' HUP; sleep 30 & exit 7", NULL, 0, &error_code));
    status = reap_with_timeout(&descendants);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CHECK(descendants.pid == -1);
    CHECK(wd_spawned_process_group_alive(&descendants));
    CHECK(wd_spawned_process_terminate_group(&descendants, 100u, 1000u, NULL, &error_code));
    CHECK(!wd_spawned_process_group_alive(&descendants));
    CHECK(descendants.process_group == -1);

    struct wd_spawned_process stubborn;
    error_code = 0;
    CHECK(wd_spawn_shell_command(&stubborn, "trap '' TERM; sleep 30", NULL, 0, &error_code));
    sleep_milliseconds(50u);
    status = 0;
    CHECK(wd_spawned_process_terminate_group(&stubborn, 100u, 1000u, &status, &error_code));
    CHECK(stubborn.pid == -1);
    CHECK(stubborn.process_group == -1);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);

    struct wd_spawned_process          invalid;
    const struct wd_process_env_change invalid_change = {
        .name   = "INVALID=NAME",
        .value  = "value",
        .action = WD_PROCESS_ENV_SET,
    };
    error_code = 0;
    CHECK(!wd_spawn_shell_command(&invalid, "true", &invalid_change, 1, &error_code));
    CHECK(error_code == EINVAL);
    CHECK(invalid.pid == -1);
    return 0;
}
