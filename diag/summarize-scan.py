# summarize-scan.py LOG: what the APE loader's MAP_FIXED mappings replaced
import collections, re, sys
segs, clob, regions = set(), collections.Counter(), collections.defaultdict(list)
procs = set()
pat = re.compile(r"(\w+) pid=(\d+) (CLOBBER|region) \[([0-9a-f]+)-([0-9a-f]+)\) tag=(\d+) prot=(\S+) share=(\d+) inh=(\d+) depth=(\d+)")
for l in open(sys.argv[1], errors="replace"):
    if l.startswith("seg pid=") and "CLOBBER" not in l:
        procs.add(l.split()[1]); continue
    m = pat.match(l)
    if not m: continue
    why, pid, kind, lo, hi, tag, prot, share, inh, depth = m.groups()
    if kind == "CLOBBER":
        clob[(tag, prot, share, depth)] += 1
        if tag != "0" or depth == "0":
            print("NOTABLE", l.rstrip())
    else:
        regions[tag].append(int(lo, 16))
print("processes:", len(procs))
for k, v in sorted(clob.items()):
    print("clobbered regions tag=%s prot=%s share=%s depth=%s: %d" % (k + (v,)))
for tag, v in sorted(regions.items(), key=lambda kv: int(kv[0])):
    near = sum(1 for a in v if 0x700000000 <= a < 0x900000000)
    print("tag %3s: %6d regions, min %x max %x, in [0x7e8,0x900) GB-ish window: %d" % (tag, len(v), min(v), max(v), near))
