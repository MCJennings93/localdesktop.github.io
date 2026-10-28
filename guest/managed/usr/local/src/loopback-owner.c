// loopback-owner: LD_PRELOAD that records which process owns each loopback TCP connection, so
// /usr/local/bin/lsof can answer `lsof -i TCP@127.0.0.1:PORT` inside the guest.
//
// Android hides /proc/net/{tcp,tcp6} from apps and refuses them sock_diag netlink sockets, so
// the real lsof can't map a TCP port to a process here. Steam needs that mapping: before it
// accepts a websocket from its UI (steamwebhelper) on 127.0.0.1 it asks lsof which process is on
// the other end, and without an answer the UI never connects and no window appears.
//
// After every successful connect() or accept() on a loopback TCP socket, the local port is
// written to /tmp/localdesktop-loopback/<port> as "pid ppid uid peer-port fd inode". The lsof
// wrapper checks that /proc/<pid>/fd/<fd> is still that socket before it answers, so a stale
// record (a closed socket, a reused port) gives no answer rather than a wrong one. Nothing else
// is changed. The idea is DroidDeck's (tools/linuxfs/preload/net.c, GPL-3.0), which answers lsof
// from inside its exec hooks instead of with a separate program.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR "/tmp/localdesktop-loopback"

// The port of a loopback IPv4/IPv6 address (v4-mapped included), or 0.
static unsigned loopback_port(const struct sockaddr *address, socklen_t length) {
  if (address->sa_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *in = (const struct sockaddr_in *) address;
    return in->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ? ntohs(in->sin_port) : 0;
  }
  if (address->sa_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) address;
    const unsigned char *b = in6->sin6_addr.s6_addr;
    int v4_loopback = IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr) && b[12] == 127 && b[13] == 0 &&
                      b[14] == 0 && b[15] == 1;
    return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr) || v4_loopback ? ntohs(in6->sin6_port) : 0;
  }
  return 0;
}

// `peer_port` is the loopback port at the other end (connect's address: a non-blocking connect
// still in progress has no peer name yet).
static void record(int fd, unsigned peer_port) {
  struct sockaddr_storage local;
  socklen_t local_length = sizeof(local);
  int type;
  socklen_t type_length = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) || type != SOCK_STREAM ||
      getsockname(fd, (struct sockaddr *) &local, &local_length)) {
    return;
  }
  unsigned port = loopback_port((struct sockaddr *) &local, local_length);
  struct stat st;
  if (!port || !peer_port || fstat(fd, &st)) return;
  char path[96], temp[112], line[128];
  snprintf(path, sizeof(path), DIR "/%u", port);
  snprintf(temp, sizeof(temp), "%s.%d", path, getpid());
  int length = snprintf(line, sizeof(line), "%d %d %d %u %d %llu\n", getpid(), getppid(),
                        (int) getuid(), peer_port, fd, (unsigned long long) st.st_ino);
  mkdir(DIR, 01777);
  int out = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) return;
  int complete = write(out, line, length) == length;
  close(out);
  if (!complete || rename(temp, path)) unlink(temp);
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
  static int (*real)(int, const struct sockaddr *, socklen_t);
  if (!real) real = (int (*)(int, const struct sockaddr *, socklen_t)) dlsym(RTLD_NEXT, "connect");
  int result = real(fd, address, length);
  // A non-blocking connect has its local port once it returns, even while still in progress.
  unsigned peer_port = address ? loopback_port(address, length) : 0;
  if ((result == 0 || errno == EINPROGRESS) && peer_port) {
    int saved = errno;
    record(fd, peer_port);
    errno = saved;
  }
  return result;
}

int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
  static int (*real)(int, struct sockaddr *, socklen_t *, int);
  if (!real) real = (int (*)(int, struct sockaddr *, socklen_t *, int)) dlsym(RTLD_NEXT, "accept4");
  int result = real(fd, address, length, flags);
  struct sockaddr_storage peer;
  socklen_t peer_length = sizeof(peer);
  if (result >= 0 && !getpeername(result, (struct sockaddr *) &peer, &peer_length)) {
    int saved = errno;
    record(result, loopback_port((struct sockaddr *) &peer, peer_length));
    errno = saved;
  }
  return result;
}

int accept(int fd, struct sockaddr *address, socklen_t *length) {
  return accept4(fd, address, length, 0);
}
