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
static int ApeScanWalk(unsigned long lo, unsigned long hi, const char *exe,
                       const char *why, int all) {
  char buf[512];
  int n, hits = 0;
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
}
