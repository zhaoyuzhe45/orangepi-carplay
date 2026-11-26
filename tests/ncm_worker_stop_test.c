#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int endpoint_fd = -1;
static atomic_int read_entered;

/* Old FunctionFS bulk endpoints report readiness but synchronous read still
 * waits for a host transfer even with O_NONBLOCK. Reproduce that boundary. */
static int endpoint_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (count == 1 && fds[0].fd == endpoint_fd) {
        fds[0].revents = POLLIN;
        return 1;
    }
    return poll(fds, count, timeout);
}

static ssize_t endpoint_read(int fd, void *buffer, size_t size)
{
    if (fd == endpoint_fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        assert(flags >= 0);
        assert(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
        atomic_store(&read_entered, 1);
    }
    return read(fd, buffer, size);
}

#define poll endpoint_poll
#define read endpoint_read
#include "../src/ncm_bridge.c"
#undef poll
#undef read

void iap2_log(const char *format, ...) { (void)format; }
static void wakeup(int sig) { (void)sig; }
static void timeout(int sig)
{
    static const char message[] = "FAIL: NCM stop blocked waiting for USB OUT read\n";
    (void)sig;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(1);
}

int main(void)
{
    struct sigaction action = {0};
    int pair[2], tun[2];
    int cycle;

    action.sa_handler = wakeup;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    action.sa_handler = timeout;
    assert(sigaction(SIGALRM, &action, NULL) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(pipe(tun) == 0);
    assert(fcntl(tun[0], F_SETFL, O_NONBLOCK) == 0);
    endpoint_fd = pair[0];
    g_tun_fd = tun[0];
    g_running = 1;
    for (cycle = 0; cycle < 20; cycle++) {
        atomic_store(&read_entered, 0);
        assert(usb_workers_start(pair[0], pair[1]) == 0);
        /* The endpoint watchdog must live exactly as long as the workers it
         * supervises; otherwise a wedged syscall has no recovery path. */
        assert(g_watchdog_started == 1);
        alarm(3);
        while (!atomic_load(&read_entered))
            usleep(1000);
        /* Also exercise signal delivery just before the syscall enters. */
        if (cycle % 2)
            usleep(10000);
        usb_workers_stop();
        assert(g_watchdog_started == 0);
        alarm(0);
    }
    g_running = 0;
    g_tun_fd = -1;
    close(tun[0]); close(tun[1]); close(pair[0]); close(pair[1]);
    puts("ncm worker stop tests passed (20 idle endpoint restarts)");
    return 0;
}
