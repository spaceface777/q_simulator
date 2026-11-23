// resv.c: does libmalloc ever map new segments over memory that someone else
// MAP_FIXED into its address reservation (as the APE loader does with a
// program image)? Map sentinels into the reservation that surrounds the
// malloc segments, allocate a lot, then check the sentinels and pointers.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct R { uint64_t lo, hi; unsigned tag, prot, share, depth; };

static int walk(struct R *out, int max) {
  int n = 0;
  mach_vm_address_t a = 0;
  natural_t depth = 0;
  while (n < max) {
    mach_vm_size_t sz = 0;
    struct vm_region_submap_info_64 info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth,
                               (vm_region_recurse_info_t)&info, &cnt))
      break;
    if (info.is_submap) { ++depth; continue; }
    out[n++] = (struct R){a, a + sz, info.user_tag, info.protection,
                          info.share_mode, depth};
    a += sz;
  }
  return n;
}

static void show(const char *why, struct R *r, int n, uint64_t lo, uint64_t hi) {
  for (int i = 0; i < n; ++i)
    if (r[i].hi > lo && r[i].lo < hi && (r[i].tag || r[i].prot || r[i].share == 3))
      printf("%s [%llx-%llx) tag=%u prot=%u share=%u depth=%u\n", why, r[i].lo,
             r[i].hi, r[i].tag, r[i].prot, r[i].share, r[i].depth);
}

#define NS 6
#define SS (32ul << 20)

int main(int argc, char **argv) {
  static char obuf[1 << 16];
  setvbuf(stdout, obuf, _IOFBF, sizeof(obuf));
  static struct R r[4096];
  int n = walk(r, 4096);
  uint64_t seg = 0, rlo = 0, rhi = 0;
  for (int i = 0; i < n; ++i)
    if (r[i].tag == 2 && r[i].depth == 0) { seg = r[i].lo; break; }
  if (!seg) { printf("no malloc small segment\n"); return 0; }
  // the reservation: the contiguous run of untagged inaccessible empty
  // regions and malloc regions around the segment
  int k;
  for (k = 0; k < n && !(r[k].lo <= seg && seg < r[k].hi); ++k) {}
  int a = k, b = k;
  while (a > 0 && r[a - 1].hi == r[a].lo && (r[a - 1].tag || r[a - 1].share == 3)) --a;
  while (b + 1 < n && r[b + 1].lo == r[b].hi && (r[b + 1].tag || r[b + 1].share == 3)) ++b;
  rlo = r[a].lo, rhi = r[b].hi;
  printf("reservation [%llx-%llx) (%llu MB), first segment %llx\n", rlo, rhi,
         (rhi - rlo) >> 20, seg);
  show("before", r, n, rlo, rhi);
  // sentinels: only in empty, untagged, inaccessible parts of the
  // reservation, at the low end of each gap (next to malloc's segments) and
  // in the middle of the largest gaps
  uint64_t s[NS] = {0};
  int ns = 0;
  for (int i = a; i <= b && ns < NS; ++i) {
    if (r[i].tag || r[i].prot || r[i].share != 3 || r[i].depth) continue;
    uint64_t lo = (r[i].lo + (16ul << 20) - 1) & -(16ul << 20);
    uint64_t mid = ((r[i].lo + r[i].hi) / 2) & -(16ul << 20);
    uint64_t c[2] = {lo, mid};
    for (int j = 0; j < 2 && ns < NS; ++j) {
      if (c[j] < r[i].lo || c[j] + SS > r[i].hi) continue;
      if (j && c[1] < c[0] + SS) continue;
      void *p = mmap((void *)c[j], SS, PROT_READ | PROT_WRITE,
                     MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
      if (p == MAP_FAILED) { printf("sentinel %llx: mmap failed\n", c[j]); continue; }
      for (uint64_t o = 0; o < SS; o += 16384) *(uint64_t *)(c[j] + o) = 0xa5a5a5a5a5a5a5a5 ^ o;
      printf("sentinel [%llx-%llx)\n", c[j], c[j] + SS);
      s[ns++] = c[j];
    }
  }
  // allocate ~1.5 GB of address space in small and medium chunks
  uint64_t total = 0, inside = 0, count = 0;
  static const size_t sizes[] = {16, 48, 128, 256, 1024, 4096, 16384, 32768, 65536};
  while (total < (1536ul << 20)) {
    size_t z = sizes[count % (sizeof(sizes) / sizeof(*sizes))];
    char *p = malloc(z);
    if (!p) { printf("malloc failed after %llu MB\n", total >> 20); break; }
    for (int i = 0; i < NS; ++i)
      if (s[i] && (uint64_t)p < s[i] + SS && (uint64_t)p + z > s[i]) {
        if (inside++ < 5) printf("MALLOC INSIDE SENTINEL %p size %zu\n", p, z);
      }
    total += z, ++count;
  }
  int bad = 0;
  for (int i = 0; i < NS; ++i) {
    if (!s[i]) continue;
    for (uint64_t o = 0; o < SS; o += 16384)
      if (*(uint64_t *)(s[i] + o) != (0xa5a5a5a5a5a5a5a5 ^ o)) { ++bad; break; }
  }
  n = walk(r, 4096);
  show("after", r, n, rlo, rhi);
  printf("allocated %llu chunks, %llu MB; pointers inside sentinels: %llu; "
         "sentinels overwritten: %d\n", count, total >> 20, inside, bad);
  return 0;
}
