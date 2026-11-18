// Linked into the diagnostic CMake only. Cosmopolitan turns on function call tracing with a
// --ftrace argument, which a program does not pass to the programs it starts. This turns it on
// from the environment instead, in `cmake -P` script processes (FetchContent's download step)
// only: QS_FTRACE=1.
#include <stdlib.h>
#include <string.h>

extern int __argc;
extern char **__argv;
int ftrace_install(void);
int ftrace_enabled(int);

__attribute__((constructor)) static void qs_ftrace(void) {
  const char *v = getenv("QS_FTRACE");
  if (!v || strcmp(v, "1"))
    return;
  for (int i = 1; i < __argc; ++i) {
    if (!strcmp(__argv[i], "-P")) {
      ftrace_install();
      ftrace_enabled(+1);
      return;
    }
  }
}
