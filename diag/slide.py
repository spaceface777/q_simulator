# slide.py < scan lines: the loader reservation's slide per process
import collections, re, sys
lo = collections.defaultdict(lambda: 1 << 64)
for l in sys.stdin:
    m = re.search(r"pid=(\d+) region \[([0-9a-f]+)-", l)
    if m: lo[m.group(1)] = min(lo[m.group(1)], int(m.group(2), 16))
v = sorted(x - 0x7f0000000 for x in lo.values())
print("reservation slide: n=%d min %#x median %#x max %#x" % (len(v), v[0], v[len(v) // 2], v[-1]) if v else "no reservation seen")
