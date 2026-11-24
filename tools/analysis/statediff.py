"""Summarize a .statediff file: which globals each eye pass changed, grouped by symbol.

Lines are "<pass> <address> <before> <after>" (dwords). Addresses are mapped to the enclosing
bw1-decomp data symbol. Globals changed in BOTH passes are the interesting ones: state that the
render pass itself mutates, so the second eye starts from different state than the first.

Usage: python tools/analysis/statediff.py <file.statediff>
"""
import bisect
import collections
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SYM = re.compile(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:object(?: size:0x([0-9A-Fa-f]+))?")


def load_objects():
    objs = []
    for line in (ROOT / "third_party/bw1-decomp/symbols_BW1W120.txt").read_text(errors="replace").splitlines():
        m = SYM.match(line)
        if m:
            objs.append((int(m.group(3), 16), int(m.group(4) or "4", 16), m.group(1)))
    objs.sort()
    return objs, [o[0] for o in objs]


def as_float(v):
    f = struct.unpack("<f", struct.pack("<I", v))[0]
    return f if abs(f) < 1e7 and (abs(f) > 1e-6 or v == 0) else None


def main(path):
    objs, starts = load_objects()
    groups = collections.defaultdict(lambda: {"L": [], "R": []})
    for line in pathlib.Path(path).read_text().splitlines():
        p, a, b, c = line.split()
        addr, before, after = int(a, 16), int(b, 16), int(c, 16)
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < objs[i][0] + max(objs[i][1], 4):
            key = f"{objs[i][2]}+0x{addr - objs[i][0]:X}"
        else:
            key = f"?{addr:08X}"
        groups[key][p].append((before, after))
    both = {k: v for k, v in groups.items() if v["L"] and v["R"]}
    only = {k: v for k, v in groups.items() if not (v["L"] and v["R"])}
    # Accumulators: the right pass continues from the left pass's result (a -> b -> c, c != a).
    # Scratch buffers just get overwritten (a -> b -> a) and don't leak state between eyes.
    accum = {k: v for k, v in both.items() if v["R"][0][1] != v["L"][0][0]}
    print(f"{len(groups)} changed locations; {len(both)} changed in both passes; {len(accum)} accumulate")
    if "--accum" in sys.argv:
        both, only = accum, {}
    for title, sel in (("changed in BOTH passes", both), ("changed in one pass", only)):
        print(f"\n== {title}")
        for k, v in sorted(sel.items()):
            desc = []
            for p in "LR":
                for before, after in v[p][:1]:
                    fb, fa = as_float(before), as_float(after)
                    desc.append(f"{p}: {fb:.5g}->{fa:.5g}" if fb is not None and fa is not None
                                else f"{p}: {before:08X}->{after:08X}")
            print(f"  {k:70s} {'  '.join(desc)}")


if __name__ == "__main__":
    main(sys.argv[1])
