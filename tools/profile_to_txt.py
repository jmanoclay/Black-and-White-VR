"""Convert a bwvr_probe JSON profile (profiles/*.json) to the key/value text the mod reads.

OpenVR eye-to-head offsets are right-handed (-Z forward); the engine is left-handed (+Z forward),
so Z is negated. Projection tangents keep OpenVR's convention (top < 0 = up).

Usage: python tools/profile_to_txt.py profiles/index.json <out.txt>
"""
import json
import sys


def main(src, dst):
    p = json.load(open(src))
    lines = [
        f"model {p['model'].replace(' ', '_')}",
        f"render_w {p['recommended_render_size'][0]}",
        f"render_h {p['recommended_render_size'][1]}",
        f"refresh_hz {p['refresh_hz']}",
        f"ipd_m {p['ipd_m']}",
    ]
    for eye in ("left", "right"):
        e = p["eyes"][eye]
        pr = e["projection_raw"]
        m = e["eye_to_head"]
        lines += [
            f"{eye}.tan_left {pr['left']}",
            f"{eye}.tan_right {pr['right']}",
            f"{eye}.tan_top {pr['top']}",
            f"{eye}.tan_bottom {pr['bottom']}",
            f"{eye}.offset_x {m[0][3]}",
            f"{eye}.offset_y {m[1][3]}",
            f"{eye}.offset_z {-m[2][3]}",
        ]
    open(dst, "w").write("\n".join(lines) + "\n")
    print(f"wrote {dst}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
