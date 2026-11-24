"""Names addresses inside a loaded DLL from its export table (the nearest export at or below).

Usage: python tools/perf/name_exports.py <dll path> <load base hex> <address hex> [...]
(The load base is the "module" line of a profile or spike capture.)
"""
import bisect
import sys

import pefile


def main():
    path, base = sys.argv[1], int(sys.argv[2], 16)
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    ex = sorted((e.address, (e.name or b"#%d" % e.ordinal).decode()) for e in pe.DIRECTORY_ENTRY_EXPORT.symbols)
    rvas = [a for a, _ in ex]
    for a in sys.argv[3:]:
        rva = int(a, 16) - base
        i = bisect.bisect_right(rvas, rva) - 1
        print(f"{a}: " + (f"{ex[i][1]}+0x{rva - ex[i][0]:X}" if i >= 0 else f"rva 0x{rva:X} (before the first export)"))


if __name__ == "__main__":
    main()
