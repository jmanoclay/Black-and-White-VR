"""Summarize D3D7 trace files written by the mod (bwvr/captures/trace_*.txt).

Resolves the recorded return addresses to bw1-decomp function names and groups draw calls by
the engine function that issued them.

Usage: python tools/analysis/trace_report.py <trace.txt> [more traces...]
"""
import bisect
import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SYMBOLS = ROOT / "third_party" / "bw1-decomp" / "symbols_BW1W120.txt"

_FUNC = re.compile(r"^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function")


def demangle_short(name):
    # ?Method@Class@@... -> Class::Method ; good enough for reports.
    m = re.match(r"\?([^@?]+)@([^@]+)@@", name)
    if m:
        return f"{m.group(2)}::{m.group(1)}"
    m = re.match(r"\?([^@?]+)@@", name)
    return m.group(1) if m else name


class Symbols:
    def __init__(self, path=SYMBOLS):
        entries = []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _FUNC.match(line)
            if m:
                entries.append((int(m.group(2), 16), demangle_short(m.group(1))))
        entries.sort()
        self.addrs = [a for a, _ in entries]
        self.names = [n for _, n in entries]

    def lookup(self, addr):
        if not (0x401000 <= addr < 0x8A9000):
            return None  # outside runblack.exe .text (e.g. d3dim700 / our proxy)
        i = bisect.bisect_right(self.addrs, addr) - 1
        return self.names[i] if i >= 0 else None


DRAW = re.compile(r"^(DP|DIP|DPS|DIPS|DPVB|DIPVB) (\S+) (\S+) (.*?) \|(.*)$")


def first_game_frames(stack, syms, n=3):
    out = []
    for tok in stack.split():
        name = syms.lookup(int(tok, 16))
        if name and (not out or out[-1] != name):
            out.append(name)
        if len(out) == n:
            break
    return out


def report(path, syms):
    draws = collections.Counter()
    prims_by_site = collections.Counter()
    fvfs = collections.Counter()
    order = []
    for line in path.read_text().splitlines():
        m = DRAW.match(line)
        if not m:
            continue
        kind, prim, fvf, rest, stack = m.groups()
        site = " < ".join(first_game_frames(stack, syms)) or "?"
        draws[site] += 1
        fvfs[fvf] += 1
        nm = re.search(r"\b(?:n|i)=(\d+)", rest)
        prims_by_site[site] += int(nm.group(1)) if nm else 0
        if not order or order[-1][0] != site:
            order.append([site, 0])
        order[-1][1] += 1
    print(f"== {path.name}: {sum(draws.values())} draws")
    print("   vertex formats:", ", ".join(f"{k}×{v}" for k, v in fvfs.most_common()))
    print("   by call site (draws, indices/verts):")
    for site, n in draws.most_common():
        print(f"     {n:5d} {prims_by_site[site]:7d}  {site}")
    print("   draw order (runs of consecutive draws from the same site):")
    for site, n in order:
        print(f"     {n:4d}  {site}")


def main(paths):
    syms = Symbols()
    for p in paths:
        report(pathlib.Path(p), syms)


if __name__ == "__main__":
    main(sys.argv[1:])
