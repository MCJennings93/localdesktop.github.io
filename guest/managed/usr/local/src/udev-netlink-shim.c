// udev-netlink-shim: LD_PRELOAD that lets libudev monitors work where Android forbids uevents.
// Android's SELinux policy denies apps NETLINK_KOBJECT_UEVENT sockets (EACCES), so
// udev_monitor_new_from_netlink() returns NULL, and KWin 6.7's GpuManager then calls
// m_udevMonitor->fd() on it and crashes (gpumanager.cpp, no null check). When the kernel refuses
// such a socket this hands out an AF_UNIX datagram socket instead, and accepts the netlink-only
// calls libudev makes on it (bind, setsockopt, getsockname), so the monitor is created and
// simply never reports a device event: there are none to report in the app anyway.
//
// Built with -DUDEV_NETLINK_SHIM_TEST, the kernel's refusal is simulated, for testing on a
// desktop where uevent sockets are allowed.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FDS 4096

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool fake_fds[MAX_FDS];

static void *real(const char *name) { return dlsym(RTLD_NEXT, name); }

static bool is_fake(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return false;
    pthread_mutex_lock(&lock);
    bool fake = fake_fds[fd];
    pthread_mutex_unlock(&lock);
    return fake;
}

static void set_fake(int fd, bool fake) {
    if (fd < 0 || fd >= MAX_FDS) return;
    pthread_mutex_lock(&lock);
    fake_fds[fd] = fake;
    pthread_mutex_unlock(&lock);
}

int socket(int domain, int type, int protocol) {
    static int (*real_socket)(int, int, int);
    if (!real_socket) real_socket = real("socket");
    int fd;
#ifdef UDEV_NETLINK_SHIM_TEST
    if (domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
        errno = EACCES;
        fd = -1;
    } else
#endif
        fd = real_socket(domain, type, protocol);
    if (fd < 0 && errno == EACCES && domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
        fd = real_socket(AF_UNIX, SOCK_DGRAM | (type & (SOCK_CLOEXEC | SOCK_NONBLOCK)), 0);
        if (fd >= 0) set_fake(fd, true);
        return fd;
    }
    if (fd >= 0) set_fake(fd, false);
    return fd;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real_bind)(int, const struct sockaddr *, socklen_t);
    if (!real_bind) real_bind = real("bind");
    if (is_fake(fd)) return 0;
    return real_bind(fd, addr, len);
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
    static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
    if (!real_setsockopt) real_setsockopt = real("setsockopt");
    if (is_fake(fd)) return 0;
    return real_setsockopt(fd, level, name, value, len);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    static int (*real_getsockname)(int, struct sockaddr *, socklen_t *);
    if (!real_getsockname) real_getsockname = real("getsockname");
    if (!is_fake(fd)) return real_getsockname(fd, addr, len);
    struct sockaddr_nl nl = {.nl_family = AF_NETLINK, .nl_pid = (unsigned)getpid()};
    if (addr && len) {
        memcpy(addr, &nl, *len < sizeof nl ? *len : sizeof nl);
        *len = sizeof nl;
    }
    return 0;
}

int close(int fd) {
    static int (*real_close)(int);
    if (!real_close) real_close = real("close");
    set_fake(fd, false);
    return real_close(fd);
}
