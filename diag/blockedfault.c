// Does XNU loop forever when a write faults while SIGSEGV/SIGBUS are blocked?
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv) {
  char *p = mmap(0, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  sigset_t all;
  sigfillset(&all);
  if (argc > 1) sigprocmask(SIG_SETMASK, &all, 0);
  fprintf(stderr, "pid %d, signals %s, writing to read-only %p\n", getpid(), argc > 1 ? "blocked" : "unblocked", p);
  memset(p, 0, 32);
  fprintf(stderr, "survived?\n");
  return 0;
}
