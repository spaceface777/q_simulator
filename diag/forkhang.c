// forkhang.c: on macOS 26 (Apple silicon), some launches of this APE
// program hang forever in fork(): the child spins at 100% CPU inside
// libSystem and never returns from fork().
//
// Why: the APE loader (ape/ape-m1.c) maps the program at its fixed link
// address, 0x800000000, with MAP_FIXED, after libSystem has initialized.
// macOS 26's malloc places its first segments at random around there, so
// now and then the program's read-only image replaces a malloc segment
// that holds objc's @synchronized records (allocated by libSystem's own
// initializer). In the fork child, objc's atfork handler frees those
// records; free() clears them, which writes to read-only program memory;
// cosmo's fork() blocks all signals, so XNU retries that write forever.
// The 256 MiB of constant data below makes the overlap likely (~2% of
// launches); a 110 MiB clang hits it in ~0.8%.
//
//   cosmocc -o forkhang forkhang.c
//   n=0; for i in $(seq 300); do ./forkhang || n=$((n+1)); done; echo "$n hung"
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

const char blob[256 << 20] = {1};  // read-only data in the program image

int main(int argc, char *argv[]) {
  int ws;
  pid_t pid = fork();
  if (!pid)
    _exit(blob[argc]);  // 0; uses blob so that the linker keeps it
  for (int i = 0; i < 300; ++i) {  // wait up to 3 seconds
    if (waitpid(pid, &ws, WNOHANG) == pid)
      return 0;
    usleep(10000);
  }
  fprintf(stderr,
          "forkhang: fork child %d hasn't returned from fork() after 3 s; "
          "it spins at 100%% CPU (`sample %d` shows _objc_atfork_child "
          "→ _xzm_free → _platform_memset)\n",
          pid, pid);
  kill(pid, SIGKILL);
  return 1;
}
