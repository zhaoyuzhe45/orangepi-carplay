#define _GNU_SOURCE
#include "carplay_display.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern void iap2_log(const char *fmt, ...);

/*
 * `wm` is a shell wrapper around app_process. Every call therefore costs a
 * process spawn plus a binder round trip to WindowManager, and when
 * system_server is mid-restart the round trip simply never returns: a single
 * `wm size` can sit in the kernel until the framework is back. This code runs
 * on the RTSP control thread, between the /info reply and the pair-setup
 * request, and the sink resets the whole session when that gap grows to
 * seconds. Measured failure it caused: a cold boot that restarted zygote under
 * a live handshake stalled the daemon 8 s in `wm density` + `wm size`, the
 * phone dropped the control connection 20 s after /info, and DiPlay parked on
 * "connecting" until the cable was replugged.
 *
 * Never wait on `wm` for longer than this; a timeout is reported as a read
 * failure, which is exactly what "the framework is not answering" means.
 */
#define WM_EXEC_TIMEOUT_MS 1500

static int parse_pair(const char *line, const char *prefix, unsigned *w,
                      unsigned *h)
{
    char tail;
    unsigned width;
    unsigned height;
    int offset = 0;

    if (strncmp(line, prefix, strlen(prefix)) != 0)
        return 0;
    line += strlen(prefix);
    if (sscanf(line, "%u%c%u%n", &width, &tail, &height,
               &offset) != 3 ||
        tail != 'x' || offset <= 0)
        return 0;
    while (line[offset] == ' ' || line[offset] == '\t' ||
           line[offset] == '\r' || line[offset] == '\n')
        offset++;
    if (line[offset] != '\0')
        return 0;
    *w = width;
    *h = height;
    return 1;
}

int carplay_display_parse_size(const char *text,
                               struct carplay_display_state *state)
{
    const char *line;
    unsigned physical_width = 0;
    unsigned physical_height = 0;
    unsigned override_width = 0;
    unsigned override_height = 0;

    if (!text || !state)
        return -1;
    for (line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        char item[160];
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len >= sizeof(item))
            len = sizeof(item) - 1u;
        memcpy(item, line, len);
        item[len] = '\0';
        if (!parse_pair(item, "Physical size: ", &physical_width,
                        &physical_height) &&
            !parse_pair(item, "Override size: ", &override_width,
                        &override_height))
            return -1;
        line = end ? end + 1 : NULL;
    }
    if (!physical_width || !physical_height)
        return -1;
    state->physical_width = physical_width;
    state->physical_height = physical_height;
    state->width = override_width ? override_width : physical_width;
    state->height = override_height ? override_height : physical_height;
    state->size_override = override_width && override_height;
    return 0;
}

int carplay_display_parse_density(const char *text,
                                  struct carplay_display_state *state)
{
    const char *line;

    if (!text || !state)
        return -1;
    for (line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        char item[128];
        unsigned value;
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len >= sizeof(item))
            len = sizeof(item) - 1u;
        memcpy(item, line, len);
        item[len] = '\0';
        if (sscanf(item, "Physical density: %u", &value) == 1) {
            if (!state->density)
                state->density = value;
            state->density_override = 0;
        } else if (sscanf(item, "Override density: %u", &value) == 1) {
            state->density = value;
            state->density_override = 1;
        } else {
            return -1;
        }
        line = end ? end + 1 : NULL;
    }
    return state->density ? 0 : -1;
}

int carplay_display_format_size_arg(unsigned width, unsigned height,
                                    char *out, size_t capacity)
{
    int n;
    if (!out || capacity == 0 || width < 320 || height < 240 ||
        width > 7680 || height > 4320 || (width & 1u) || (height & 1u))
        return -1;
    n = snprintf(out, capacity, "%ux%u", width, height);
    return n > 0 && (size_t)n < capacity ? 0 : -1;
}

int carplay_display_format_density_arg(unsigned density,
                                       char *out, size_t capacity)
{
    int n;
    if (!out || capacity == 0 || density < 120 || density > 480)
        return -1;
    n = snprintf(out, capacity, "%u", density);
    return n > 0 && (size_t)n < capacity ? 0 : -1;
}

int carplay_display_format_physical_mode(unsigned width, unsigned height,
                                         unsigned fps, char *out,
                                         size_t capacity)
{
    int n;
    if (!out || capacity == 0 || width < 320 || height < 240 ||
        width > 7680 || height > 4320 || (width & 1u) || (height & 1u) ||
        fps < 1 || fps > 120)
        return -1;
    /* WxH@fps is the format both apply_display_mode.sh (`now WxH@fps`) and the
     * Rockchip property parser accept - `persist.sys.framebuffer.main=1920x900@60`
     * was verified to switch the composer on this unit, while the `WxHp fps`
     * variant this used to emit was rejected by the script outright. */
    n = snprintf(out, capacity, "%ux%u@%u", width, height, fps);
    return n > 0 && (size_t)n < capacity ? 0 : -1;
}

/* Wait for a child without ever blocking indefinitely. Returns 0 when the child
 * was reaped, -1 when it had to be killed or waitpid broke. */
static int wait_child_timeout(pid_t pid, int *status, int timeout_ms)
{
    int waited = 0;

    for (;;) {
        pid_t rc = waitpid(pid, status, WNOHANG);

        if (rc == pid)
            return 0;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (waited >= timeout_ms) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, status, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
        usleep(10000);
        waited += 10;
    }
}

static int read_pipe_command(const char *const argv[], char *out,
                             size_t capacity)
{
    int pipe_fds[2];
    pid_t pid;
    size_t used = 0;
    int status = 0;

    if (!out || capacity < 2 || pipe(pipe_fds) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipe_fds[1]);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    for (;;) {
        struct pollfd pfd;
        ssize_t n;
        int ready;

        if (used + 1u >= capacity)
            break;
        pfd.fd = pipe_fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, WM_EXEC_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            break;
        n = read(pipe_fds[0], out + used, capacity - used - 1u);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    out[used] = '\0';
    close(pipe_fds[0]);
    if (wait_child_timeout(pid, &status, WM_EXEC_TIMEOUT_MS) < 0)
        return -1;

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int read_wm(const char *mode, char *out, size_t capacity)
{
    const char *argv[] = { "/system/bin/sh", "/system/bin/wm",
                           mode, NULL };
    return read_pipe_command(argv, out, capacity);
}

static int read_text_file(const char *path, char *out, size_t capacity)
{
    int fd;
    size_t len = 0;

    if (!out || capacity < 2)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (len + 1u < capacity) {
        ssize_t n = read(fd, out + len, capacity - len - 1u);
        if (n > 0) {
            len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);
    out[len] = '\0';
    return 0;
}

static void trim_eol(char *text)
{
    size_t len;

    if (!text)
        return;
    len = strlen(text);
    while (len && (text[len - 1u] == '\n' || text[len - 1u] == '\r' ||
                   text[len - 1u] == ' ' || text[len - 1u] == '\t'))
        text[--len] = '\0';
}

int carplay_display_probe(struct carplay_display_state *state)
{
    char output[1024];

    if (!state)
        return -1;
    memset(state, 0, sizeof(*state));
    if (read_wm("size", output, sizeof(output)) < 0)
        return -1;
    return carplay_display_parse_size(output, state);
}

/* Detached jobs inherit neither transport sockets nor input/encoder devices.
 * Only the intermediate child is reaped here; Android work stays off RTSP. */
static int launch_display_helper(const char *const argv[], int *completion_fd)
{
    int completion[2] = {-1, -1};
    if (completion_fd) {
        *completion_fd = -1;
        if (pipe2(completion, O_CLOEXEC|O_NONBLOCK) < 0) return -1;
    }
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 1024;
    pid_t pid = fork();
    if (pid < 0) {
        if (completion[0] >= 0) { close(completion[0]); close(completion[1]); }
        return -1;
    }
    if (pid == 0) {
        if (completion[0] >= 0) close(completion[0]);
        if (setsid() < 0) _exit(127);
        pid_t child = fork();
        if (child != 0) _exit(child < 0 ? 127 : 0);
        int fd = open("/dev/null", O_RDWR);
        if (fd < 0) _exit(127);
        (void)dup2(fd, STDIN_FILENO);
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
        fd = open("/data/carplay/logs/display-mode.log", O_WRONLY|O_CREAT|O_APPEND, 0600);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        for (long i = 3; i < max_fd; i++)
            if (i != completion[1]) close((int)i);
        if (completion[1] >= 0) {
            /* This detached supervisor owns no stream/input descriptors.
             * Report exec/script/sync success, not merely a successful fork. */
            pid_t executor = fork();
            if (executor != 0) {
                int child_status = 0;
                pid_t waited = -1;
                if (executor > 0) {
                    do { waited = waitpid(executor, &child_status, 0); }
                    while (waited < 0 && errno == EINTR);
                }
                unsigned char result = waited == executor && executor > 0 &&
                    WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0 ? 1 : 2;
                (void)write(completion[1], &result, 1);
                close(completion[1]);
                _exit(0);
            }
            close(completion[1]);
        }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (completion[1] >= 0) close(completion[1]);
    int status = 0;
    pid_t reaped;
    do { reaped = waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    int success = reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (completion[0] >= 0) {
        if (success) *completion_fd = completion[0];
        else close(completion[0]);
    }
    return success ? 0 : -1;
}

int carplay_display_start_preparation(const struct carplay_display_plan *plan,
                                      char *request_id, size_t capacity)
{
    char mode[32], density[16], id[64];
    struct timespec stamp;
    if (!plan || !request_id || capacity < sizeof(id) ||
        carplay_display_format_physical_mode(plan->physical_width, plan->physical_height,
            plan->physical_fps, mode, sizeof(mode)) < 0 ||
        !carplay_display_mode_builtin(plan->physical_width, plan->physical_height) ||
        plan->physical_fps != 60 || plan->density < 120 || plan->density > 480 ||
        carplay_display_preparation_state(plan->physical_width, plan->physical_height,
            plan->physical_fps) != 0 ||
        access("/system/etc/carplay/apply_display_mode.sh", R_OK) != 0 ||
        carplay_display_format_density_arg(plan->density, density, sizeof(density)) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return -1;
    snprintf(id, sizeof(id), "cp-%ld-%ld-%ld", (long)getpid(),
             (long)stamp.tv_sec, (long)stamp.tv_nsec);
    const char *argv[] = {"/system/bin/sh", "/system/etc/carplay/apply_display_mode.sh",
                         "prepare", mode, density, id, "--media-stopped", NULL};
    if (launch_display_helper(argv, NULL) < 0) return -1;
    snprintf(request_id, capacity, "%s", id);
    return 0;
}

int carplay_display_request_mode(unsigned width, unsigned height, unsigned fps,
                                 unsigned density)
{
    struct carplay_display_plan plan = {0};
    char request_id[64];
    plan.physical_width=width; plan.physical_height=height;
    plan.physical_fps=fps; plan.density=density;
    return carplay_display_start_preparation(&plan, request_id, sizeof(request_id));
}

static int start_remember_mode(unsigned width, unsigned height, unsigned density,
                              int *completion_fd)
{
    char mode[32], density_arg[16];
    if (!carplay_display_mode_builtin(width,height) || density < 120 || density > 480 ||
        carplay_display_format_physical_mode(width,height,60,mode,sizeof(mode)) < 0 ||
        carplay_display_format_density_arg(density,density_arg,sizeof(density_arg)) < 0 ||
        access("/system/etc/carplay/apply_display_mode.sh",R_OK) != 0) return -1;
    const char *argv[] = {"/system/bin/sh", "/system/etc/carplay/apply_display_mode.sh",
                         "remember", mode, density_arg, NULL};
    return launch_display_helper(argv, completion_fd);
}

int carplay_display_remember_mode(unsigned width, unsigned height, unsigned density)
{
    return start_remember_mode(width, height, density, NULL);
}

void carplay_display_remember_close(struct carplay_display_memory *memory)
{
    if (!memory || !memory->pending) return;
    close(memory->fd);
    memory->fd = -1;
    memory->pending = 0;
}

void carplay_display_remember_step(struct carplay_display_memory *memory,
    unsigned width, unsigned height, unsigned density, uint64_t now_ms)
{
    if (!memory || memory->complete || now_ms < memory->next_ms) return;
    memory->next_ms = now_ms + 1000u;
    if (memory->pending) {
        unsigned char result = 0;
        ssize_t n = read(memory->fd, &result, 1);
        if (n < 0 && (errno == EINTR || errno == EAGAIN) && now_ms < memory->deadline_ms)
            return;
        memory->complete = n == 1 && result == 1;
        carplay_display_remember_close(memory);
        iap2_log("display: remember completion=%d attempt=%u preset=%ux%u",
                 memory->complete, memory->attempts, width, height);
        return;
    }
    if (memory->attempts >= 3) return;
    memory->attempts++;
    if (start_remember_mode(width, height, density, &memory->fd) == 0) {
        memory->pending = 1;
        memory->deadline_ms = now_ms + 45000u;
    }
    iap2_log("display: remember dispatched=%d attempt=%u preset=%ux%u",
             memory->pending, memory->attempts, width, height);
}

static int status_value(const char *text, const char *key, char *out, size_t capacity)
{
    size_t keylen=strlen(key);
    int found=0;
    for (const char *line=text; line && *line;) {
        const char *end=strchr(line,'\n');
        size_t len=end ? (size_t)(end-line) : strlen(line);
        if (len && line[len-1]=='\r') len--;
        if (len > keylen && !memcmp(line,key,keylen) && line[keylen]=='=') {
            size_t value_len=len-keylen-1;
            if (found || value_len >= capacity) return -1;
            memcpy(out,line+keylen+1,value_len); out[value_len]=0; found=1;
        }
        line=end ? end+1 : NULL;
    }
    return found;
}

int carplay_display_parse_preparation_result(const char *text, const char *boot_id,
    const char *request_id, unsigned width, unsigned height, unsigned fps)
{
    char version[8], boot[80], id[64], mode[32], expected[32], phase[32], result[16];
    if (!text || !boot_id || !request_id || !*request_id ||
        carplay_display_format_physical_mode(width,height,fps,expected,sizeof(expected)) < 0 ||
        status_value(text,"version",version,sizeof(version)) != 1 || strcmp(version,"1") ||
        status_value(text,"boot_id",boot,sizeof(boot)) != 1 || strcmp(boot,boot_id) ||
        status_value(text,"request_id",id,sizeof(id)) != 1 || strcmp(id,request_id) ||
        status_value(text,"mode",mode,sizeof(mode)) != 1 || strcmp(mode,expected) ||
        status_value(text,"phase",phase,sizeof(phase)) != 1 ||
        status_value(text,"result",result,sizeof(result)) != 1) return 0;
    if (!strcmp(phase,"complete") && !strcmp(result,"0")) return 1;
    if (!strcmp(phase,"failed")) return -1;
    return 0;
}

int carplay_display_preparation_result(const char *request_id, unsigned width,
                                       unsigned height, unsigned fps)
{
    char text[2048],boot[80];
    if (read_text_file("/proc/sys/kernel/random/boot_id",boot,sizeof(boot)) < 0 ||
        read_text_file("/data/carplay/.display-mode-status",text,sizeof(text)) < 0) return 0;
    trim_eol(boot);
    return carplay_display_parse_preparation_result(text,boot,request_id,width,height,fps);
}

int carplay_display_preparation_state(unsigned width, unsigned height, unsigned fps)
{
    char boot[80], path[160], failed[4096], target[128];
    struct stat info;
    if (read_text_file("/proc/sys/kernel/random/boot_id", boot, sizeof(boot)) < 0)
        return 1;
    trim_eol(boot);
    if (!boot[0] || strchr(boot, '/') || strchr(boot, '\\')) return 1;
    snprintf(path, sizeof(path), "/data/carplay/.display-mode-lock-%s", boot);
    if (stat(path, &info) == 0) return 1;
    if (!width || !height) return 0;
    snprintf(target, sizeof(target), "%s %ux%u@%u", boot, width, height, fps);
    if (read_text_file("/data/carplay/.display-mode-failed-targets", failed, sizeof(failed)) == 0) {
        char *save = NULL;
        for (char *line = strtok_r(failed, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
            if (strcmp(line, target) == 0) return -1;
    }
    return 0;
}

int carplay_display_boot_provisioning_pending(void)
{
    return carplay_display_preparation_state(0,0,0) != 0;
}

int carplay_display_mode_builtin(unsigned width, unsigned height)
{
    static const unsigned modes[][2] = {
        { 800u, 480u }, { 1024u, 480u }, { 1280u, 600u },
        { 1280u, 720u }, { 1920u, 900u }, { 1920u, 1080u }
    };
    size_t i;

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (modes[i][0] == width && modes[i][1] == height)
            return 1;
    }
    return 0;
}

int carplay_display_mode_supported(const char *text, unsigned width,
                                   unsigned height, unsigned fps)
{
    const char *line;

    if (!text || !width || !height)
        return 0;
    for (line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        char item[160];
        unsigned item_width = 0;
        unsigned item_height = 0;
        unsigned item_fps = 0;
        char marker = '\0';
        size_t len = end ? (size_t)(end - line) : strlen(line);

        if (len >= sizeof(item))
            len = sizeof(item) - 1u;
        memcpy(item, line, len);
        item[len] = '\0';
        if (sscanf(item, "%ux%u%c%u", &item_width, &item_height,
                   &marker, &item_fps) >= 2 &&
            item_width == width && item_height == height &&
            (!marker ||
             ((marker != 'p' && marker != 'i' && marker != '@') ||
              item_fps == fps)))
            return 1;
        line = end ? end + 1 : NULL;
    }
    return 0;
}

int carplay_display_read(struct carplay_display_state *state)
{
    char system_output[1024];
    char density_output[512];

    if (!state)
        return -1;
    memset(state, 0, sizeof(*state));
    if (read_wm("size", system_output, sizeof(system_output)) < 0) {
        iap2_log("display: wm size read failed");
        return -1;
    }
    if (read_wm("density", density_output, sizeof(density_output)) < 0) {
        iap2_log("display: wm density read failed");
        return -1;
    }
    if (carplay_display_parse_size(system_output, state) < 0) {
        iap2_log("display: wm size output rejected=%s", system_output);
        return -1;
    }
    if (carplay_display_parse_density(density_output, state) < 0) {
        iap2_log("display: wm density output rejected=%s", density_output);
        return -1;
    }
    return 0;
}

/* Framework readback is bounded. Physical preparation runs only after the
 * bridge retires media; the early-boot profile needs no framework restart. */
int carplay_display_framework_ready(void)
{
    struct carplay_display_state probe;
    char size_output[1024];

    if (read_wm("size", size_output, sizeof(size_output)) < 0)
        return 0;
    memset(&probe, 0, sizeof(probe));
    return carplay_display_parse_size(size_output, &probe) == 0;
}

static int run_wm(const char *mode, const char *value)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (value) {
            char *const argv[] = { "/system/bin/sh",
                                   "/system/bin/wm", (char *)mode,
                                   (char *)value, NULL };
            execv(argv[0], argv);
        } else {
            char *const argv[] = { "/system/bin/sh",
                                   "/system/bin/wm", (char *)mode, NULL };
            execv(argv[0], argv);
        }
        _exit(127);
    }
    if (wait_child_timeout(pid, &status, WM_EXEC_TIMEOUT_MS) < 0)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int carplay_display_restore(const struct carplay_display_result *result)
{
    char size_arg[32];
    char density_arg[16];

    if (!result)
        return -1;
    if (result->prior_size_override) {
        if (carplay_display_format_size_arg(result->prior_width,
                                            result->prior_height, size_arg,
                                            sizeof(size_arg)) < 0 ||
            run_wm("size", size_arg) < 0)
            return -1;
    } else if (run_wm("size", "reset") < 0) {
        return -1;
    }
    if (result->prior_density_override) {
        if (carplay_display_format_density_arg(result->prior_density,
                                               density_arg,
                                               sizeof(density_arg)) < 0 ||
            run_wm("density", density_arg) < 0)
            return -1;
    } else if (run_wm("density", "reset") < 0) {
        return -1;
    }
    return 0;
}

int carplay_display_apply(const struct carplay_display_plan *plan,
                          struct carplay_display_result *result)
{
    struct carplay_display_state prior;
    struct carplay_display_state current;
    char size_arg[32];
    char density_arg[16];
    int attempt;
    int success = 0;
    int wrote = 0;

    if (!plan || !result)
        return -1;
    memset(result, 0, sizeof(*result));
    if (carplay_display_format_size_arg(plan->width, plan->height,
                                        size_arg, sizeof(size_arg)) < 0 ||
        carplay_display_format_density_arg(plan->density, density_arg,
                                           sizeof(density_arg)) < 0) {
        snprintf(result->error, sizeof(result->error),
                 "invalid display plan");
        return -1;
    }
    if (carplay_display_read(&prior) < 0) {
        snprintf(result->error, sizeof(result->error),
                 "cannot read current display");
        return -1;
    }
    result->prior_width = prior.width;
    result->prior_height = prior.height;
    result->prior_physical_width = prior.physical_width;
    result->prior_physical_height = prior.physical_height;
    result->prior_density = prior.density;
    result->prior_size_override = prior.size_override;
    result->prior_density_override = prior.density_override;
    result->width = prior.width;
    result->height = prior.height;
    result->physical_width = prior.physical_width;
    result->physical_height = prior.physical_height;
    result->density = prior.density;

    if (plan->force_physical) {
        int panel_matches = prior.physical_width == plan->physical_width &&
                            prior.physical_height == plan->physical_height;

        if (!panel_matches) {
            /* A logical override cannot repair a physical mismatch. The
             * bridge retires this session and prepares the preset separately. */
            iap2_log("display: panel runs %ux%u, preset is %ux%u; physical preparation required",
                     prior.physical_width, prior.physical_height,
                     plan->physical_width, plan->physical_height);
            snprintf(result->error, sizeof(result->error), "physical mode preparation required");
            return 1;
        }
        if (panel_matches) {
            if (prior.size_override && run_wm("size", "reset") < 0) {
                snprintf(result->error, sizeof(result->error),
                         "wm size reset failed");
                goto restore;
            }
            if (prior.size_override) { result->changed = 1; wrote = 1; }
        } else if (prior.width != plan->width ||
                   prior.height != plan->height) {
            if (run_wm("size", size_arg) < 0) {
                snprintf(result->error, sizeof(result->error),
                         "wm size failed");
                goto restore;
            }
            result->changed = 1;
            wrote = 1;
        }
    } else if (prior.width != plan->width || prior.height != plan->height) {
        if (run_wm("size", size_arg) < 0) {
            snprintf(result->error, sizeof(result->error),
                     "wm size failed");
            goto restore;
        }
        result->changed = 1;
        wrote = 1;
    }
    if (prior.density != plan->density) {
        if (run_wm("density", density_arg) < 0) {
            snprintf(result->error, sizeof(result->error),
                     "wm density failed");
            goto restore;
        }
        wrote = 1;
    }

    /* Nothing was written, so a readback would only sleep. This runs on the
     * RTSP main loop between /info and pair-setup; the log shows the no-op
     * path used to burn 2.4 s on every reconnect that needed no change
     * (02:24:16.566 -> 02:24:19.099, changed=0), which is pure handshake
     * latency against a sink that resets the session when it stalls. The
     * prior state was read at entry, so it already IS the applied state. */
    if (!wrote) {
        result->width = prior.width;
        result->height = prior.height;
        result->physical_width = prior.physical_width;
        result->physical_height = prior.physical_height;
        result->density = prior.density;
        result->native = prior.physical_width == plan->width &&
                         prior.physical_height == plan->height &&
                         !prior.size_override;
        snprintf(result->error, sizeof(result->error), "%s", "ok");
        iap2_log("display: already at the plan; readback skipped native=%d size=%ux%u density=%u",
                 result->native, result->width, result->height, result->density);
        return 0;
    }

    /* Readback budget stays short on purpose: this runs on the RTSP main loop,
     * and the phone resets the session if the handshake stalls for seconds. */
    for (attempt = 0; attempt < 12; attempt++) {
        usleep(200000);
        if (carplay_display_read(&current) == 0 &&
            current.width == plan->width &&
            current.height == plan->height &&
            current.density == plan->density &&
            (!plan->force_physical ||
             (current.physical_width == plan->physical_width &&
              current.physical_height == plan->physical_height && !current.size_override))) {
            result->width = current.width;
            result->height = current.height;
            result->physical_width = current.physical_width;
            result->physical_height = current.physical_height;
            result->density = current.density;
            /* native means 1:1 composition: the panel already runs at the car
             * canvas size, so nothing is rescaled in the composer. */
            result->native = current.physical_width == plan->width &&
                             current.physical_height == plan->height &&
                             !current.size_override;
            snprintf(result->error, sizeof(result->error), "%s", "ok");
            iap2_log("display: switched native=%d physical=%ux%u size=%ux%u density=%u changed=%d",
                     result->native, result->physical_width,
                     result->physical_height, result->width, result->height,
                     result->density, result->changed);
            return 0;
        }
    }
    snprintf(result->error, sizeof(result->error),
             "display readback did not match requested plan");
restore:
    success = carplay_display_restore(result);
    iap2_log("display: switch failed error=%s rollback=%s",
             result->error, success == 0 ? "ok" : "failed");
    return -1;
}

/* ---- panel-mirror policy --------------------------------------------------
 *
 * The panel is pinned at its native mode for the whole life of the box and the
 * canvas is expressed as a WindowManager size override at the canvas aspect.
 * screenrecord then captures the PANEL 1:1 (verified on this unit: the virtual
 * display receives the panel's composition scaled to the requested size, so a
 * capture size different from the panel only produces letterboxed, rescaled
 * frames). The composer renders a logical display smaller than the panel
 * scaled UP and centered (measured: wm 1280x600 on the 1920x1080 panel
 * composes at 0,90-1920,990), so the override is the fill rectangle - the
 * largest canvas-aspect rectangle contained in the panel - which pins the
 * composer scale at exactly 1.0, and the SPS frame-cropping rewrite hands the
 * sink exactly the centered viewport region. Nothing is ever rescaled by the
 * composer or the encoder, and no framework restart is ever needed for a
 * canvas change. */

int carplay_display_mirror_size(unsigned canvas_width, unsigned canvas_height,
                                unsigned panel_width, unsigned panel_height,
                                unsigned *mirror_width, unsigned *mirror_height)
{
    unsigned canvas_w = canvas_width & ~1u;
    unsigned canvas_h = canvas_height & ~1u;
    unsigned mirror_w;
    unsigned mirror_h;

    if (!mirror_width || !mirror_height)
        return -1;
    *mirror_width = 0;
    *mirror_height = 0;
    if (!canvas_w || !canvas_h || !panel_width || !panel_height)
        return -1;
    /* A canvas bigger than the panel cannot be composed at 1:1 at all - no
     * override adds pixels to the panel. Fitting it inside the panel instead
     * leaves bars in the stream that the sink has to undo by enlarging, and a
     * sink that lays the stream out at its own canvas size shows that
     * letterboxed picture inset. Compose the whole panel and let the sink fill
     * its canvas with it: the picture arrives stretched where the aspects
     * differ, which is the deliberate trade for a full-panel stream on a
     * request the panel cannot represent. */
    if (canvas_w > panel_width || canvas_h > panel_height) {
        *mirror_width = panel_width & ~1u;
        *mirror_height = panel_height & ~1u;
        return 0;
    }
    /* No "canvas already fits" shortcut: SurfaceFlinger on this unit renders
     * a logical display smaller than the panel scaled UP to the largest
     * canvas-aspect rectangle, centered (measured: wm 1280x600 on the
     * 1920x1080 panel composes at 0,90-1920,990). The WindowManager override
     * must be that fill rectangle so the composer scale is exactly 1.0 and
     * the SPS crop only has to remove the centered bars. */
    {
        uint64_t by_width = (uint64_t)canvas_h * panel_width;
        uint64_t by_height = (uint64_t)canvas_w * panel_height;

        /* Whichever bound is tighter fixes the scale; the other axis floors.
         * Even dimensions: the encoder and the SPS crop units both step in
         * multiples of two. */
        if (by_width <= by_height) {
            mirror_w = panel_width;
            mirror_h = (unsigned)(by_width / canvas_w);
        } else {
            mirror_h = panel_height;
            mirror_w = (unsigned)(by_height / canvas_h);
        }
    }
    mirror_w &= ~1u;
    mirror_h &= ~1u;
    if (mirror_w < 2u)
        mirror_w = 2u;
    if (mirror_h < 2u)
        mirror_h = 2u;
    if (mirror_w > panel_width)
        mirror_w = panel_width & ~1u;
    if (mirror_h > panel_height)
        mirror_h = panel_height & ~1u;
    *mirror_width = mirror_w;
    *mirror_height = mirror_h;
    return 0;
}

int carplay_display_letterbox_origin(unsigned panel_width,
                                     unsigned panel_height,
                                     unsigned mirror_width,
                                     unsigned mirror_height, unsigned *x,
                                     unsigned *y)
{
    if (!x || !y || !panel_width || !panel_height || !mirror_width ||
        !mirror_height || mirror_width > panel_width ||
        mirror_height > panel_height)
        return -1;
    /* Even origin: the SPS crop offsets step in chroma samples of two. */
    *x = ((panel_width - mirror_width) / 2u) & ~1u;
    *y = ((panel_height - mirror_height) / 2u) & ~1u;
    return 0;
}

int carplay_display_viewport_rect(unsigned panel_width, unsigned panel_height,
                                  unsigned logical_width,
                                  unsigned logical_height, unsigned *x,
                                  unsigned *y, unsigned *width,
                                  unsigned *height)
{
    unsigned mirror_w;
    unsigned mirror_h;

    if (!width || !height)
        return -1;
    if (carplay_display_mirror_size(logical_width, logical_height,
                                    panel_width, panel_height, &mirror_w,
                                    &mirror_h) < 0)
        return -1;
    /* Capture coordinates, NOT panel coordinates.  screenrecord's virtual
     * display samples the DISPLAY FRAME, not the panel: measured on this
     * unit with wm 1920x900 on a 1920x1080 panel, a 1920x1080 capture puts
     * the frame's first row at capture y=0 (a constant +90 against the
     * panel, slope 1.0 for 250 rows verified pixel-wise) and leaves the
     * rows below the frame unwritten.  The panel letterbox offset
     * (carplay_display_letterbox_origin) therefore must NOT be applied
     * here: cropping at +90 removes the first 90 rows of the picture
     * itself, which is exactly the "picture rides up, bottom goes blank"
     * symptom. */
    *x = 0;
    *y = 0;
    *width = mirror_w;
    *height = mirror_h;
    return 0;
}

int carplay_display_panel_point(unsigned stream_x, unsigned stream_y,
                                unsigned viewport_width,
                                unsigned viewport_height,
                                unsigned letterbox_x, unsigned letterbox_y,
                                unsigned panel_width, unsigned panel_height,
                                unsigned *x, unsigned *y)
{
    uint64_t px;
    uint64_t py;

    if (!x || !y || !viewport_width || !viewport_height || !panel_width ||
        !panel_height)
        return -1;
    /* Clamp inside the viewport first: a sink that reports a coordinate one
     * past the edge must land on the last visible pixel, not on the
     * letterbox bar outside the logical display. */
    px = stream_x < viewport_width ? stream_x : viewport_width - 1u;
    py = stream_y < viewport_height ? stream_y : viewport_height - 1u;
    px += letterbox_x;
    py += letterbox_y;
    if (px >= panel_width)
        px = panel_width - 1u;
    if (py >= panel_height)
        py = panel_height - 1u;
    *x = (unsigned)px;
    *y = (unsigned)py;
    return 0;
}

int carplay_display_input_coordinate(unsigned seen_x, unsigned seen_y,
                                     unsigned stream_origin_x,
                                     unsigned stream_origin_y,
                                     unsigned viewport_origin_x,
                                     unsigned viewport_origin_y,
                                     unsigned viewport_width,
                                     unsigned viewport_height,
                                     unsigned logical_width,
                                     unsigned logical_height, unsigned *x,
                                     unsigned *y)
{
    int64_t px;
    int64_t py;

    if (!x || !y || !viewport_width || !viewport_height || !logical_width ||
        !logical_height)
        return -1;
    /* The seen pixel sits at seen + (stream origin - viewport origin) inside
     * the composed viewport rectangle; the viewport is the logical display
     * scaled by viewport/logical. Invert both. */
    px = (int64_t)seen_x + (int64_t)stream_origin_x -
         (int64_t)viewport_origin_x;
    py = (int64_t)seen_y + (int64_t)stream_origin_y -
         (int64_t)viewport_origin_y;
    if (px < 0)
        px = 0;
    if (py < 0)
        py = 0;
    *x = (unsigned)(((uint64_t)px * logical_width + viewport_width / 2u) /
                    viewport_width);
    *y = (unsigned)(((uint64_t)py * logical_height + viewport_height / 2u) /
                    viewport_height);
    return 0;
}

unsigned carplay_display_mirror_density(unsigned canvas_width,
                                        unsigned canvas_height,
                                        unsigned canvas_density,
                                        unsigned mirror_width,
                                        unsigned mirror_height)
{
    unsigned canvas_h = canvas_height & ~1u;
    unsigned density;

    (void)mirror_width;
    if (!canvas_width || !canvas_h || !canvas_density || !mirror_height)
        return 0;
    density = (unsigned)(((uint64_t)canvas_density * mirror_height +
                          canvas_h / 2u) / canvas_h);
    if (density < 120u)
        density = 120u;
    if (density > 480u)
        density = 480u;
    return density;
}

int carplay_display_apply_mirror(unsigned canvas_width, unsigned canvas_height,
                                 unsigned canvas_density,
                                 struct carplay_display_result *result)
{
    struct carplay_display_state prior;
    struct carplay_display_state current;
    char size_arg[32];
    char density_arg[16];
    unsigned mirror_w;
    unsigned mirror_h;
    unsigned density;
    int attempt;
    int changed = 0;
    int success;

    if (!result)
        return -1;
    memset(result, 0, sizeof(*result));
    if (carplay_display_read(&prior) < 0) {
        snprintf(result->error, sizeof(result->error),
                 "cannot read current display");
        return -1;
    }
    if (carplay_display_mirror_size(canvas_width, canvas_height,
                                    prior.physical_width,
                                    prior.physical_height, &mirror_w,
                                    &mirror_h) < 0) {
        snprintf(result->error, sizeof(result->error), "invalid canvas");
        return -1;
    }
    density = carplay_display_mirror_density(canvas_width, canvas_height,
                                             canvas_density, mirror_w,
                                             mirror_h);
    if (!density ||
        carplay_display_format_size_arg(mirror_w, mirror_h, size_arg,
                                        sizeof(size_arg)) < 0 ||
        carplay_display_format_density_arg(density, density_arg,
                                           sizeof(density_arg)) < 0) {
        snprintf(result->error, sizeof(result->error), "invalid mirror plan");
        return -1;
    }
    result->prior_width = prior.width;
    result->prior_height = prior.height;
    result->prior_physical_width = prior.physical_width;
    result->prior_physical_height = prior.physical_height;
    result->prior_density = prior.density;
    result->prior_size_override = prior.size_override;
    result->prior_density_override = prior.density_override;
    result->width = mirror_w;
    result->height = mirror_h;
    result->density = density;
    result->physical_width = prior.physical_width;
    result->physical_height = prior.physical_height;

    if (prior.width != mirror_w || prior.height != mirror_h ||
        (mirror_w == prior.physical_width &&
         mirror_h == prior.physical_height && prior.size_override)) {
        /* A mirror equal to the panel must not carry an override at all - a
         * leftover one would letterbox the composition against the panel. */
        const char *arg = mirror_w == prior.physical_width &&
                                  mirror_h == prior.physical_height
                              ? "reset"
                              : size_arg;

        if (run_wm("size", arg) < 0) {
            snprintf(result->error, sizeof(result->error), "wm size failed");
            goto restore;
        }
        changed = 1;
    }
    if (prior.density != density && run_wm("density", density_arg) < 0) {
        snprintf(result->error, sizeof(result->error), "wm density failed");
        goto restore;
    }

    /* Readback budget stays short on purpose: this runs on the RTSP main
     * loop, and the phone resets the session if the handshake stalls. */
    for (attempt = 0; attempt < 12; attempt++) {
        usleep(200000);
        if (carplay_display_read(&current) == 0 &&
            current.width == mirror_w && current.height == mirror_h &&
            current.density == density) {
            result->changed = changed;
            result->native = mirror_w == current.physical_width &&
                             mirror_h == current.physical_height &&
                             !current.size_override;
            snprintf(result->error, sizeof(result->error), "%s", "ok");
            iap2_log("display: mirror panel=%ux%u canvas=%ux%u size=%ux%u density=%u changed=%d",
                     current.physical_width, current.physical_height,
                     canvas_width, canvas_height, mirror_w, mirror_h,
                     density, changed);
            return 0;
        }
    }
    snprintf(result->error, sizeof(result->error),
             "mirror readback did not match the plan");
restore:
    success = carplay_display_restore(result);
    iap2_log("display: mirror switch failed error=%s rollback=%s",
             result->error, success == 0 ? "ok" : "failed");
    return -1;
}
