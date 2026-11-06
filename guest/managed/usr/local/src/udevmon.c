/*
 * A stand-in for udev's netlink socket, built into libfakeinput.so.
 *
 * From DroidDeck 4f58230a tools/linuxfs/preload/udevmon.c (and the bind and recvmsg hooks from its
 * net.c and ntsync.c), ported from WinNative (maxjivi05), GPL-3.0.
 *
 * Android refuses an app socket(AF_NETLINK, ..., NETLINK_KOBJECT_UEVENT), and libudev's
 * udev_monitor_new_from_netlink fails with it. SDL before 2.0.14 then gives up on joysticks
 * altogether ("Could not initialize UDEV"), so a game bundling such an SDL (Bastion's is from 2018)
 * sees no controller. Here the refused socket becomes one end of a Unix datagram pair that answers
 * bind, getsockname, setsockopt and recvmsg the way a netlink socket would, and udev messages are
 * written to the other end:
 *  - bl_udevmon_inject (libfakeinput calls it for the Deck's hidraw device in the Steam client);
 *  - a watcher on FAKE_EVDEV_DIR: when Steam's virtual pad /dev/input/eventN (N >= 16) appears or
 *    goes, every stand-in in this process gets an add or remove for it, with the ID_INPUT_JOYSTICK
 *    property a udev rule would add. libfakeinput writes the pad's sysfs and udev entries before the
 *    node appears.
 * Wine keeps its own fallback for a refused socket, so Wine processes get the refusal.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define UINPUT_EVENT_BASE 16
#define UINPUT_EVENT_LIMIT 64
#define EVENT_MINOR_BASE 64

struct stand_in {
  int fd;
  int peer;
  ino_t inode;
};

static struct stand_in stand_ins[16];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t watcher_once = PTHREAD_ONCE_INIT;

static void lock_before_fork(void) { pthread_mutex_lock(&lock); }
static void unlock_after_fork(void) { pthread_mutex_unlock(&lock); }
static void reset_after_fork(void) {
  pthread_mutex_init(&lock, NULL);
  /* The watcher thread does not survive fork: a child that opens a monitor starts its own. */
  watcher_once = (pthread_once_t)PTHREAD_ONCE_INIT;
}

__attribute__((constructor)) static void init_udevmon(void) {
  for (size_t i = 0; i < sizeof(stand_ins) / sizeof(stand_ins[0]); i++) stand_ins[i].fd = -1;
  pthread_atfork(lock_before_fork, unlock_after_fork, reset_after_fork);
}

static ino_t socket_inode(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? st.st_ino : 0;
}

static int is_stand_in(int fd) {
  int found = 0;
  if (fd < 0) return 0;
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < sizeof(stand_ins) / sizeof(stand_ins[0]); i++) {
    if (stand_ins[i].fd != fd) continue;
    if (stand_ins[i].inode == socket_inode(fd)) {
      found = 1;
    } else {
      /* The program closed the stand-in and the descriptor number was reused. */
      close(stand_ins[i].peer);
      stand_ins[i].fd = -1;
    }
    break;
  }
  pthread_mutex_unlock(&lock);
  return found;
}

static int is_wine_process(void) {
  static int known;
  if (known == 0) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *name = exe;
    if (n > 0) {
      exe[n] = '\0';
      name = strrchr(exe, '/');
      name = name != NULL ? name + 1 : exe;
    }
    known = n > 0 && strncmp(name, "wine", 4) == 0 ? 1 : -1;
  }
  return known == 1;
}

static void *watch_pads(void *unused);

static void start_watcher(void) {
  pthread_t thread;
  if (getenv("FAKE_EVDEV_DIR") == NULL) return;
  if (pthread_create(&thread, NULL, watch_pads, NULL) == 0) pthread_detach(thread);
}

static int make_stand_in(int type) {
  int sv[2];
  int saved = errno;
  int flags = SOCK_DGRAM | (type & (SOCK_CLOEXEC | SOCK_NONBLOCK));
  if (socketpair(AF_UNIX, flags, 0, sv) != 0) {
    errno = saved;
    return -1;
  }
  fcntl(sv[1], F_SETFD, FD_CLOEXEC);
  fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < sizeof(stand_ins) / sizeof(stand_ins[0]); i++) {
    if (stand_ins[i].fd == -1) {
      stand_ins[i].fd = sv[0];
      stand_ins[i].peer = sv[1];
      stand_ins[i].inode = socket_inode(sv[0]);
      pthread_mutex_unlock(&lock);
      pthread_once(&watcher_once, start_watcher);
      return sv[0];
    }
  }
  pthread_mutex_unlock(&lock);
  close(sv[0]);
  close(sv[1]);
  errno = saved;
  return -1;
}

__attribute__((visibility("default"))) int socket(int domain, int type, int protocol) {
  static int (*real_socket)(int, int, int);
  int fd;

  if (real_socket == NULL) real_socket = (int (*)(int, int, int))dlsym(RTLD_NEXT, "socket");
  if (real_socket == NULL) {
    errno = ENOSYS;
    return -1;
  }
  fd = real_socket(domain, type, protocol);
  if (fd >= 0 || domain != AF_NETLINK || protocol != NETLINK_KOBJECT_UEVENT) return fd;
  int refused = errno;
  if (is_wine_process()) {
    errno = refused;
    return -1;
  }
  fd = make_stand_in(type);
  if (fd < 0) errno = refused;
  return fd;
}

__attribute__((visibility("default"))) int bind(int fd, const struct sockaddr *addr, socklen_t len) {
  static int (*real_bind)(int, const struct sockaddr *, socklen_t);
  if (is_stand_in(fd)) {
    if (addr == NULL || len < sizeof(struct sockaddr_nl) || addr->sa_family != AF_NETLINK) {
      errno = EINVAL;
      return -1;
    }
    return 0;
  }
  if (real_bind == NULL) real_bind = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "bind");
  if (real_bind == NULL) {
    errno = ENOSYS;
    return -1;
  }
  return real_bind(fd, addr, len);
}

__attribute__((visibility("default"))) int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
  static int (*real_getsockname)(int, struct sockaddr *, socklen_t *);
  struct sockaddr_nl nl;

  if (is_stand_in(fd)) {
    if (addr == NULL || len == NULL) {
      errno = EFAULT;
      return -1;
    }
    memset(&nl, 0, sizeof(nl));
    nl.nl_family = AF_NETLINK;
    nl.nl_pid = (unsigned)getpid();
    memcpy(addr, &nl, *len < sizeof(nl) ? *len : sizeof(nl));
    *len = sizeof(nl);
    return 0;
  }
  if (real_getsockname == NULL)
    real_getsockname = (int (*)(int, struct sockaddr *, socklen_t *))dlsym(RTLD_NEXT, "getsockname");
  if (real_getsockname == NULL) {
    errno = ENOSYS;
    return -1;
  }
  return real_getsockname(fd, addr, len);
}

__attribute__((visibility("default"))) int setsockopt(int fd, int level, int optname, const void *optval,
                                                      socklen_t optlen) {
  static int (*real_setsockopt)(int, int, int, const void *, socklen_t);

  if (is_stand_in(fd)) {
    if (level == SOL_NETLINK) return 0;
    if (level == SOL_SOCKET && (optname == SO_ATTACH_FILTER || optname == SO_DETACH_FILTER ||
                                optname == SO_RCVBUFFORCE))
      return 0;
  }
  if (real_setsockopt == NULL)
    real_setsockopt = (int (*)(int, int, int, const void *, socklen_t))dlsym(RTLD_NEXT, "setsockopt");
  if (real_setsockopt == NULL) {
    errno = ENOSYS;
    return -1;
  }
  return real_setsockopt(fd, level, optname, optval, optlen);
}

/* A message from udevd: libudev only trusts it from the multicast group 2 (UDEV_MONITOR_UDEV) and from
 * uid 0, so the sender is filled in on the way out. */
__attribute__((visibility("default"))) ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
  static ssize_t (*real_recvmsg)(int, struct msghdr *, int);
  if (real_recvmsg == NULL) real_recvmsg = (ssize_t (*)(int, struct msghdr *, int))dlsym(RTLD_NEXT, "recvmsg");
  if (real_recvmsg == NULL) {
    errno = ENOSYS;
    return -1;
  }
  if (msg == NULL || !is_stand_in(fd)) return real_recvmsg(fd, msg, flags);
  size_t control_room = msg->msg_controllen;
  socklen_t name_room = msg->msg_namelen;
  ssize_t n = real_recvmsg(fd, msg, flags);
  if (n < 0) return n;
  if (msg->msg_name != NULL && name_room >= sizeof(struct sockaddr_nl)) {
    struct sockaddr_nl nl;
    memset(&nl, 0, sizeof(nl));
    nl.nl_family = AF_NETLINK;
    nl.nl_groups = 2;
    memcpy(msg->msg_name, &nl, sizeof(nl));
    msg->msg_namelen = sizeof(nl);
  }
  if (msg->msg_control != NULL && control_room >= CMSG_SPACE(sizeof(struct ucred))) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    struct ucred cred = {.pid = 1, .uid = 0, .gid = 0};
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    memcpy(CMSG_DATA(cmsg), &cred, sizeof(cred));
    msg->msg_controllen = CMSG_SPACE(sizeof(struct ucred));
  } else {
    msg->msg_controllen = 0;
  }
  return n;
}

struct monitor_netlink_header {
  char prefix[8];
  unsigned magic;
  unsigned header_size;
  unsigned properties_off;
  unsigned properties_len;
  unsigned filter_subsystem_hash;
  unsigned filter_devtype_hash;
  unsigned filter_tag_bloom_hi;
  unsigned filter_tag_bloom_lo;
};

static unsigned murmur_hash2(const char *key, size_t len) {
  const unsigned m = 0x5bd1e995;
  unsigned h = (unsigned)len;
  const unsigned char *data = (const unsigned char *)key;
  while (len >= 4) {
    unsigned k;
    memcpy(&k, data, 4);
    k *= m;
    k ^= k >> 24;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }
  switch (len) {
  case 3: h ^= data[2] << 16; /* fallthrough */
  case 2: h ^= data[1] << 8; /* fallthrough */
  case 1: h ^= data[0]; h *= m;
  }
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

/* extra: more NUL-separated KEY=value properties (and its length), or NULL. */
static int inject(const char *action, const char *devpath, const char *subsystem, const char *devname,
                  unsigned major, unsigned minor, const char *extra, size_t extra_len) {
  static unsigned long long seqnum = 900000;
  char props[1024];
  int used = snprintf(props, sizeof(props), "ACTION=%s%cDEVPATH=%s%cSUBSYSTEM=%s%cDEVNAME=%s%cMAJOR=%u%cMINOR=%u%cSEQNUM=%llu",
                      action, 0, devpath, 0, subsystem, 0, devname, 0, major, 0, minor, 0,
                      __atomic_add_fetch(&seqnum, 1, __ATOMIC_RELAXED));
  if (used <= 0 || (size_t)used >= sizeof(props)) return -1;
  used++;
  if (extra != NULL) {
    if ((size_t)used + extra_len > sizeof(props)) return -1;
    memcpy(props + used, extra, extra_len);
    used += (int)extra_len;
  }
  struct monitor_netlink_header header;
  memset(&header, 0, sizeof(header));
  memcpy(header.prefix, "libudev", 8);
  header.magic = htobe32(0xfeedcafe);
  header.header_size = sizeof(header);
  header.properties_off = sizeof(header);
  header.properties_len = (unsigned)used;
  header.filter_subsystem_hash = htobe32(murmur_hash2(subsystem, strlen(subsystem)));
  char message[sizeof(header) + sizeof(props)];
  memcpy(message, &header, sizeof(header));
  memcpy(message + sizeof(header), props, (size_t)used);
  int sent = 0;
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < sizeof(stand_ins) / sizeof(stand_ins[0]); i++) {
    if (stand_ins[i].fd == -1) continue;
    if (send(stand_ins[i].peer, message, sizeof(header) + (size_t)used, MSG_DONTWAIT | MSG_NOSIGNAL) > 0) sent++;
  }
  pthread_mutex_unlock(&lock);
  return sent;
}

__attribute__((visibility("default"))) int bl_udevmon_inject(const char *action, const char *devpath,
                                                             const char *subsystem, const char *devname,
                                                             unsigned major, unsigned minor) {
  return inject(action, devpath, subsystem, devname, major, minor, NULL, 0);
}

static void pad_uevent(const char *action, int node) {
  static const char extra[] = "ID_INPUT=1\0ID_INPUT_JOYSTICK=1";
  char devpath[64], devname[32];
  snprintf(devpath, sizeof(devpath), "/devices/virtual/input/input%d/event%d", node, node);
  snprintf(devname, sizeof(devname), "/dev/input/event%d", node);
  inject(action, devpath, "input", devname, 13, EVENT_MINOR_BASE + (unsigned)node, extra, sizeof(extra));
}

static int pad_node(const char *name) {
  char *end;
  long node;
  if (strncmp(name, "event", 5) != 0 || name[5] < '0' || name[5] > '9') return -1;
  node = strtol(name + 5, &end, 10);
  return *end == '\0' && node >= UINPUT_EVENT_BASE && node < UINPUT_EVENT_LIMIT ? (int)node : -1;
}

static void *watch_pads(void *unused) {
  const char *dir = getenv("FAKE_EVDEV_DIR");
  char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  (void)unused;
  /* Raw syscalls: libfakeinput's inotify_add_watch rewrites /dev/input, and this must not recurse. */
  int fd = (int)syscall(SYS_inotify_init1, IN_CLOEXEC);
  if (fd < 0) return NULL;
  if (syscall(SYS_inotify_add_watch, fd, dir, IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM) < 0) {
    close(fd);
    return NULL;
  }
  for (;;) {
    ssize_t n = syscall(SYS_read, fd, buffer, sizeof(buffer));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    for (char *p = buffer; p < buffer + n;) {
      struct inotify_event *event = (struct inotify_event *)p;
      p += sizeof(*event) + event->len;
      int node = event->len ? pad_node(event->name) : -1;
      if (node < 0) continue;
      pad_uevent(event->mask & (IN_CREATE | IN_MOVED_TO) ? "add" : "remove", node);
    }
  }
  close(fd);
  return NULL;
}
