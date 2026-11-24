"""Summarise a sampling profile written by the mod (stereo profile <ms> -> captures\\profile.txt).

Each sample is the game thread's instruction pointer and a few return addresses up its
frame-pointer chain, tagged with the part of the VR frame it was in. Addresses in runblack.exe
are named from the bw1-decomp symbol list; others by their module.

Prints, for the chosen parts of the frame, the functions where the time is spent (self) and the
functions the time is spent under (inclusive, counted once per sample).

Usage: python tools/perf/profile_report.py [profile.txt] [--parts 2,3] [--top 30]
  parts: 0 outside the 3D pass, 1 setup/after, 2 left eye, 3 right eye, 4 readback
"""
import argparse
import bisect
import collections
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYMBOLS = ROOT / "third_party" / "bw1-decomp" / "symbols_BW1W120.txt"
DEFAULT = Path(r"D:\Games\Black & White\bwvr\captures\profile.txt")
PART_NAMES = {0: "outside the 3D pass", 1: "setup/after", 2: "left eye", 3: "right eye", 4: "readback"}


def load_symbols():
    syms = []
    for line in open(SYMBOLS, encoding="utf-8", errors="replace"):
        m = re.match(r"(\S+) = \.text:0x([0-9A-Fa-f]+);.*?size:0x([0-9A-Fa-f]+)", line)
        if m:
            syms.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    syms.sort()
    return syms, [s[0] for s in syms]


# DLLs whose functions are named from their PDBs: (module name, prefix, the DLL as built).
PDB_MODULES = [
    ("ddraw.dll", "mod", ROOT / "build" / "RelWithDebInfo" / "ddraw.dll"),
    ("dxwrapper.dll", "dxw", Path(r"D:\Git\dxwrapper\bin\Release\dxwrapper.dll")),
]


class ModSymbols:
    """Names addresses in the mod's own DLL (the game folder's ddraw.dll) and dxwrapper from their PDBs, via dbghelp."""

    def __init__(self, modules):
        import ctypes
        from ctypes import wintypes
        self.ok = False
        self.cache = {}
        self.loaded = []  # (base, size, prefix)
        self.dbg = ctypes.WinDLL("dbghelp")
        self.proc = wintypes.HANDLE(0x1234)  # (a pseudo handle: nothing is read from a live process)
        self.dbg.SymSetOptions(0x2 | 0x4)  # SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS
        self.dbg.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
        path = ";".join(str(dll.parent) for _, _, dll in PDB_MODULES)
        if not self.dbg.SymInitializeW(self.proc, path, False):
            return
        self.dbg.SymLoadModuleExW.restype = ctypes.c_uint64
        self.dbg.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_uint64,
                                              wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        for name, prefix, dll in PDB_MODULES:
            # (the mod's ddraw.dll is the one over 1 MB, not the system's)
            hits = [(b, sz) for b, sz, m in modules if m.lower() == name and (name != "ddraw.dll" or sz > 0x100000)]
            if hits and dll.exists() and self.dbg.SymLoadModuleExW(self.proc, None, str(dll), None, hits[0][0], hits[0][1], None, 0):
                self.loaded.append((hits[0][0], hits[0][1], prefix))
        if not self.loaded:
            return

        class SYMBOL_INFOW(ctypes.Structure):
            _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                        ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wintypes.ULONG),
                        ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG),
                        ("Tag", wintypes.ULONG), ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", wintypes.WCHAR * 256)]
        self.SYMBOL_INFOW = SYMBOL_INFOW
        self.ctypes = ctypes
        self.ok = True

    def name(self, a):
        if not self.ok:
            return None
        prefix = next((p for b, sz, p in self.loaded if b <= a < b + sz), None)
        if not prefix:
            return None
        if a in self.cache:
            return self.cache[a]
        info = self.SYMBOL_INFOW()
        info.SizeOfStruct = 88  # (sizeof(SYMBOL_INFOW) with its one-character name)
        info.MaxNameLen = 255
        disp = self.ctypes.c_uint64(0)
        n = f"{prefix}:{info.Name}" if self.dbg.SymFromAddrW(self.proc, self.ctypes.c_uint64(a), self.ctypes.byref(disp), self.ctypes.byref(info)) else None
        self.cache[a] = n
        return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", default=str(DEFAULT))
    ap.add_argument("--parts", default="2,3")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--slow-ms", type=float, default=0,
                    help="only the frames that took longer than this (samples are 0.25 ms apart; needs the frame numbers)")
    args = ap.parse_args()
    parts = {int(p) for p in args.parts.split(",")}
    syms, addrs = load_symbols()
    modules = []
    samples = []
    for line in open(args.file, encoding="utf-8", errors="replace"):
        f = line.split()
        if f[0] == "module":
            modules.append((int(f[1], 16), int(f[2], 16), " ".join(f[3:])))
        elif f[0] == "s":
            frame = int(f[2][1:]) if len(f) > 2 and f[2].startswith("f") else -1
            stack = [int(x, 16) for x in f[2:] if not x.startswith("f")]
            samples.append((int(f[1]), stack, frame))
    if args.slow_ms > 0:
        per_frame = collections.Counter(fr for _, _, fr in samples)
        slow = {fr for fr, c in per_frame.items() if c * 0.25 > args.slow_ms and fr >= 0}
        print(f"{len(per_frame)} frames, {len(slow)} over {args.slow_ms} ms: "
              + ", ".join(f"{per_frame[fr] * 0.25:.0f}" for fr in sorted(slow)))
        samples = [s for s in samples if s[2] in slow]
    samples = [(p, st) for p, st, _ in samples]

    mod_symbols = ModSymbols(modules)

    def name(a):
        for base, size, mod in modules:
            if base <= a < base + size and mod.lower() != "runblack.exe":
                return mod_symbols.name(a) or f"[{mod}]"
        k = bisect.bisect_right(addrs, a) - 1
        if k >= 0 and syms[k][0] <= a < syms[k][0] + max(syms[k][1], 1):
            return syms[k][2]
        return f"?{a:08X}"

    by_part = collections.Counter(p for p, _ in samples)
    total = len(samples)
    print(f"{total} samples: " + ", ".join(f"{PART_NAMES.get(p, p)} {n} ({100 * n / total:.0f}%)" for p, n in sorted(by_part.items())))
    chosen = [(p, st) for p, st in samples if p in parts]
    n = len(chosen) or 1
    self_t = collections.Counter(name(st[0]) for _, st in chosen if st)
    incl = collections.Counter()
    for _, st in chosen:
        for fn in {name(a) for a in st}:
            incl[fn] += 1
    print(f"\nparts {sorted(parts)}: {len(chosen)} samples\n\nself:")
    for fn, c in self_t.most_common(args.top):
        print(f"  {100 * c / n:5.1f}%  {fn[:110]}")
    # Time in system DLLs (the driver, Windows), put on the first caller that is the game's, the mod's or dxwrapper's.
    via = collections.Counter()
    for _, st in chosen:
        if not st or not name(st[0]).startswith("["):
            continue
        caller = next((name(a) for a in st[1:] if not name(a).startswith("[")), "(no named caller)")
        via[f"{name(st[0])} <- {caller}"] += 1
    print("\nin system DLLs, by caller:")
    for fn, c in via.most_common(args.top):
        print(f"  {100 * c / n:5.1f}%  {fn[:140]}")
    print("\ninclusive:")
    for fn, c in incl.most_common(args.top):
        print(f"  {100 * c / n:5.1f}%  {fn[:110]}")


if __name__ == "__main__":
    main()
