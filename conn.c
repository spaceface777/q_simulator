// conn <mode> <count>: curl's connect sequence, <count> times in this process.
// mode: dns    getaddrinfo("github.com") first (cosmo's UDP resolver), then connect
//       nodns  connect to 140.82.112.4 directly
//       bind   as dns, with an explicit bind(0.0.0.0:0) before connect
// Prints one line per attempt: ok, or the connect() error.
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int attempt(const char *mode) {
  struct sockaddr_in sin = {.sin_family = AF_INET, .sin_port = htons(443)};
  inet_pton(AF_INET, "140.82.112.4", &sin.sin_addr);
  if (strcmp(mode, "nodns")) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    int rc = getaddrinfo("github.com", "443", &hints, &res);
    if (rc) {
      printf("getaddrinfo: %s\n", gai_strerror(rc));
      return 1;
    }
    memcpy(&sin, res->ai_addr, sizeof(sin));
    freeaddrinfo(res);
  }
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
  if (fd == -1) {
    printf("socket: %s\n", strerror(errno));
    return 1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (!strcmp(mode, "bind")) {
    struct sockaddr_in any = {.sin_family = AF_INET};
    if (bind(fd, (void *)&any, sizeof(any)))
      printf("bind: %s\n", strerror(errno));
  }
  char ip[32];
  inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof(ip));
  int failed = 0;
  if (connect(fd, (void *)&sin, sizeof(sin)) && errno != EINPROGRESS) {
    printf("fd %d connect %s: %s\n", fd, ip, strerror(errno));
    failed = 1;
  } else {
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    int err = 0;
    socklen_t len = sizeof(err);
    if (poll(&p, 1, 10000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
      printf("fd %d connect %s: completion failed: %s\n", fd, ip, strerror(err ? err : errno));
      failed = 1;
    } else {
      printf("fd %d connect %s: ok\n", fd, ip);
    }
  }
  close(fd);
  return failed;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "dns";
  int count = argc > 2 ? atoi(argv[2]) : 1, failed = 0;
  setvbuf(stdout, 0, _IOLBF, 0);
  for (int i = 0; i < count; ++i)
    failed += attempt(mode);
  printf("RESULT %s: %d of %d failed\n", mode, failed, count);
  return failed != 0;
}
