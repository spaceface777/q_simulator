// stand-in for the hang: the fork child writes to a read-only page with
// every signal blocked, so XNU re-faults forever; the parent waits.
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
  char *p = mmap(0, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  sigset_t all;
  sigfillset(&all);
  if (!fork()) {
    sigprocmask(SIG_SETMASK, &all, 0);
    memset(p, 0, 32);
    _exit(0);
  }
  int ws;
  wait(&ws);
  return 0;
}
