// Tests connect() on an unbound socket, mainly on Windows: Winsock needs
// a socket bound before it connects, so the libc binds it to the wildcard
// address first. Built with GCC 15 and later, that bind's address was the
// family and stack garbage ({0} sets only the first member of a union),
// and connect() failed with "Address not available" or "Address already
// in use" (NOTES.md, "Unions initialized with {0}"). Needs no network:
//
//     cosmocc -o connect_test connect_test.c
//     ./connect_test
//
// Connects to a listener on 127.0.0.1, blocking and non-blocking, with
// the stack filled with nonzero bytes first, so that garbage fails every
// time and not now and then. Prints one line per check, and exits 1 if
// any check fails.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ROUNDS 10

static int failures;

static void expect(int ok, const char *what, int err) {
  if (ok)
    printf("ok   %s\n", what);
  else
    printf("FAIL %s: %s\n", what, strerror(err));
  failures += !ok;
}

// leaves nonzero bytes where connect() will put its locals
__attribute__((noipa)) static void dirty(void) {
  volatile char x[16384];
  for (int i = 0; i < sizeof(x); ++i)
    x[i] = 0x5a;
}

// connects to `to` from an unbound socket; returns 0 or an errno
__attribute__((noipa)) static int attempt(const struct sockaddr_in *to,
                                          int nonblock,
                                          struct sockaddr_in *from) {
  int fd, err = 0;
  socklen_t len;
  if ((fd = socket(AF_INET, SOCK_STREAM | (nonblock ? SOCK_NONBLOCK : 0),
                   IPPROTO_TCP)) == -1)
    return errno;
  if (connect(fd, (const struct sockaddr *)to, sizeof(*to))) {
    if (!nonblock || errno != EINPROGRESS) {
      err = errno;
    } else {
      struct pollfd p = {.fd = fd, .events = POLLOUT};
      len = sizeof(err);
      if (poll(&p, 1, 10000) != 1)
        err = ETIMEDOUT;
      else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len))
        err = errno;
    }
  }
  len = sizeof(*from);
  if (!err && getsockname(fd, (struct sockaddr *)from, &len))
    err = errno;
  close(fd);
  return err;
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  struct sockaddr_in to = {.sin_family = AF_INET};
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(to);
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server == -1 || bind(server, (struct sockaddr *)&to, sizeof(to)) ||
      listen(server, 2 * ROUNDS) ||
      getsockname(server, (struct sockaddr *)&to, &len)) {
    perror("listener");
    return 2;
  }
  for (int nonblock = 0; nonblock < 2; ++nonblock) {
    int err = 0, round;
    struct sockaddr_in from;
    for (round = 0; round < ROUNDS && !err; ++round) {
      dirty();
      memset(&from, 0, sizeof(from));
      err = attempt(&to, nonblock, &from);
      if (!err && (from.sin_family != AF_INET ||
                   from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
                   !from.sin_port))
        err = EADDRNOTAVAIL;  // bound to something other than ours
      int peer = accept(server, 0, 0);
      if (peer != -1)
        close(peer);
    }
    char what[96];
    snprintf(what, sizeof(what), "%s connect() from an unbound socket%s",
             nonblock ? "non-blocking" : "blocking",
             err ? "" : ", 10 times");
    expect(!err, what, err);
  }
  close(server);
  printf("%s\n", failures ? "FAILED" : "PASSED");
  return !!failures;
}
