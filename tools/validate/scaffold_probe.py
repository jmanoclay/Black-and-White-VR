"""Debug probe: a held scaffold over another, its +0x90 / +0x68 and the game's "applies" answer
at a few places (the joining test's helper; player, 2026-10-30).
Usage: python tools/validate/scaffold_probe.py <held hex> <target hex> x z [x z ...]"""
import re, sys, time
import near_hand as nh
import leash_direct as ld

def peek(addr, off):
    mark = nh.LOG.stat().st_size
    ld.send(f"hands peek {int(addr, 16) + off:X} 1")
    time.sleep(0.4)
    m = re.findall(r"peek: \S+: (\w+)", ld.tail(mark))
    return m[-1] if m else "?"

held, target = sys.argv[1], sys.argv[2]
pts = [float(v) for v in sys.argv[3:]]
for i in range(0, len(pts), 2):
    x, z = pts[i], pts[i + 1]
    for dy in (3.0, 6.0, 10.0):
        g = nh.ground_at(x, z)
        ld.place_palm((x, g + dy, z))
        time.sleep(1.0)
        mark = nh.LOG.stat().st_size
        ld.send(f"hands objects {x:.1f} {z:.1f} 5", "hands bp_log 1")
        time.sleep(1.0)
        t = ld.tail(mark)
        ap = [l for l in t.splitlines() if target[-6:] in l and "hands: object" in l]
        bp = re.findall(r"hands: blueprint (valid|none)", t)
        print(f"({x:.1f}, {z:.1f}) +{dy}: held +0x90 {peek(held, 0x90)} +0x68 {peek(held, 0x68)}; target +0x90 {peek(target, 0x90)} +0x68 {peek(target, 0x68)}; "
              f"applies {' applies' in ap[0] if ap else '?'}; blueprint {bp}")
