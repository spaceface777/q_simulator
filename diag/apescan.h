/* Diagnostic for the macOS 26 fork hang: before the APE loader maps a
   program segment with MAP_FIXED, log every existing region that the
   mapping would replace. With APE_SCAN_ALL=1, also log the whole address
   space once per process (to learn where libSystem's malloc puts memory).
   Logs go to the file named by APE_SCAN_LOG (appended); nothing otherwise. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static int ApeScanFd = -2;
static char **ApeScanArgv;
extern char **environ;
#include <mach-o/dyld.h>

static void ApeScanArgs(int argc, char **argv, char **envp) {
  (void)argc;
  (void)envp;
  ApeScanArgv = argv;
}

static int ApeScanOpen(void) {
  if (ApeScanFd == -2) {
    const char *f = getenv("APE_SCAN_LOG");
    ApeScanFd = f ? open(f, O_WRONLY | O_APPEND | O_CREAT, 0644) : -1;
  }
  return ApeScanFd;
}

static void ApeScanPut(const char *s, int n) {
  if (ApeScanOpen() >= 0)
    write(ApeScanFd, s, n);
}

/* walks regions intersecting [lo,hi); calls back with each */
static int ApeScanLive, ApeScanMalloc; /* per walk: live regions, tag 1-13 */

static int ApeScanWalk(unsigned long lo, unsigned long hi, const char *exe,
                       const char *why, int all) {
  char buf[512];
  int n, hits = 0;
  ApeScanLive = ApeScanMalloc = 0;
  mach_vm_address_t a = lo;
  natural_t depth = 0;
  for (;;) {
    mach_vm_size_t sz = 0;
    struct vm_region_submap_info_64 info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(
        mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info,
        &cnt);
    if (kr != KERN_SUCCESS || a >= hi)
      break;
    if (info.is_submap) {
      ++depth;
      continue;
    }
    if (a + sz <= lo) {
      a += sz;
      continue;
    }
    ++hits;
    if (info.protection || info.user_tag)
      ++ApeScanLive; /* anything but an inaccessible untagged reservation */
    if (info.user_tag >= 1 && info.user_tag <= 13)
      ++ApeScanMalloc;
    n = snprintf(buf, sizeof(buf),
                 "%s pid=%d %s [%llx-%llx) tag=%u prot=%d/%d share=%u "
                 "inh=%u depth=%u exe=%s\n",
                 why, getpid(), all ? "region" : "CLOBBER", a, a + sz,
                 info.user_tag, info.protection, info.max_protection,
                 info.share_mode, info.inheritance, depth, exe);
    ApeScanPut(buf, n);
    a += sz;
  }
  return hits;
}

static void ApeScan(unsigned long lo, unsigned long hi, const char *exe) {
  static int once;
  char buf[256];
  int n;
  if (ApeScanOpen() < 0)
    return;
  if (!once) {
    once = 1;
    if (getenv("APE_SCAN_ALL"))
      ApeScanWalk(0, -1ul, exe, "all", 1);
  }
  n = snprintf(buf, sizeof(buf), "seg pid=%d [%lx-%lx) exe=%s\n", getpid(),
               lo, hi, exe);
  ApeScanPut(buf, n);
  ApeScanWalk(lo, hi, exe, "seg", 0);
  /* APE_SCAN_POLICY=avoid: re-exec while a live region would be replaced;
     =force: re-exec until the first segment replaces a malloc region */
  const char *pol = getenv("APE_SCAN_POLICY");
  if (!pol)
    return;
  static int first = 1;
  int isfirst = first;
  first = 0;
  int again = 0;
  if (!strcmp(pol, "avoid") && ApeScanLive)
    again = 1;
  if (!strcmp(pol, "force") && isfirst && !ApeScanMalloc)
    again = 1;
  const char *t = getenv("APE_SCAN_TRIES");
  int tries = t ? atoi(t) : 0;
  if (!again) {
    if (tries) {
      n = snprintf(buf, sizeof(buf), "policy pid=%d %s ok after %d re-execs exe=%s\n",
                   getpid(), pol, tries, exe);
      ApeScanPut(buf, n);
      unsetenv("APE_SCAN_TRIES");
    }
    if (!strcmp(pol, "force"))
      unsetenv("APE_SCAN_POLICY"); /* one-shot: not for the programs it runs */
    return;
  }
  if (tries >= 5000) {
    n = snprintf(buf, sizeof(buf), "policy pid=%d %s GAVE UP exe=%s\n", getpid(), pol, exe);
    ApeScanPut(buf, n);
    return;
  }
  char self[4096], num[16];
  uint32_t len = sizeof(self);
  if (_NSGetExecutablePath(self, &len))
    return;
  snprintf(num, sizeof(num), "%d", tries + 1);
  setenv("APE_SCAN_TRIES", num, 1);
  if (!strcmp(pol, "avoid")) {
    n = snprintf(buf, sizeof(buf), "policy pid=%d avoid RE-EXEC try %d exe=%s\n", getpid(), tries + 1, exe);
    ApeScanPut(buf, n);
  }
  execve(self, ApeScanArgv, environ);
}
