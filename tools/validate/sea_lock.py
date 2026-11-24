"""World-lock check for the sea, which the engine draws as a screen-space backdrop.

The sea is not geometry: each frame the engine unprojects screen rows onto the plane y = 0 and
derives texture coordinates from that (fn_00879930). If that is done right, the sea texture moves
across the image exactly like a world-fixed plane at y = 0 would when the head moves.

Captures must be taken with `sea_only 1` (only the sea is drawn; everything else stays magenta)
and a frozen scene. For each eye, corners of the sea texture in A are lifted onto the plane, projected
into B, and tracked there; the residual is how far the texture slides against the world.

Usage: python tools/validate/sea_lock.py <capA> <capB> [plane_y]
"""
import json
import sys

import numpy as np

from stereo_checks import MAGENTA, load, refine


def plane_points(eye, pts, plane_y):
    rx, ry = eye.to_render(pts[:, 0], pts[:, 1])
    tx = (rx - eye.render_w / 2) / eye.f
    ty = -(ry - eye.render_h / 2) / eye.f
    d = eye.R[0] * tx[:, None] + eye.R[1] * ty[:, None] + eye.R[2]
    t = (plane_y - eye.pos[1]) / d[:, 1]
    return eye.pos + d * t[:, None], t > 0


def sea_points(eye, n=2500):
    import cv2
    mask = (~np.all(np.abs(eye.img.astype(int) - MAGENTA) <= 2, axis=2)).astype(np.uint8)
    mask = cv2.erode(mask, np.ones((25, 25), np.uint8))
    g = cv2.cvtColor(eye.img, cv2.COLOR_RGB2GRAY)
    p = cv2.goodFeaturesToTrack(g, n, 0.005, 10, mask=mask)
    return np.zeros((0, 2)) if p is None else p.reshape(-1, 2)


def check(A, B, plane_y):
    pts = sea_points(A)
    if len(pts) < 20:
        return {"points": int(len(pts)), "pass": False, "note": "too few sea corners"}
    import cv2
    world, front = plane_points(A, pts, plane_y)
    pred, Z = B.project(world)
    guess = pred - [B.cx0, B.cy0]
    # The plane y = plane_y maps A to B by an exact homography. Warp B back into A's frame with it
    # so the tracker only has to find the residual (robust to large rotations and foreshortening).
    gy, gx = np.mgrid[A.ch * 0.45:A.ch:40j, 0:A.cw:40j]
    grid = np.c_[gx.ravel(), gy.ravel()]
    gw, gf = plane_points(A, grid, plane_y)
    gp, gz = B.project(gw)
    gok = gf & (gz > 0)
    H, _ = cv2.findHomography(grid[gok], gp[gok] - [B.cx0, B.cy0])
    b_in_a = cv2.warpPerspective(B.img, H, (A.cw, A.ch), flags=cv2.WARP_INVERSE_MAP | cv2.INTER_LINEAR,
                                 borderValue=(255, 0, 255))
    found_a, ok = refine(A.img, b_in_a, pts, pts.copy())
    res = found_a - pts  # residual slip, in A pixels
    inside = (guess[:, 0] > 15) & (guess[:, 1] > 15) & (guess[:, 0] < B.cw - 15) & (guess[:, 1] < B.ch - 15)
    v = ok & front & (Z > 0) & inside
    err = np.linalg.norm(res, axis=1)[v]
    if len(err) < 20:
        return {"points": int(len(pts)), "tracked": int(v.sum()), "pass": False}
    # Residual by screen band (top = near the horizon) shows where the texture slides.
    rows = pts[v, 1]
    bands = []
    for lo, hi in np.percentile(rows, [[0, 25], [25, 50], [50, 75], [75, 100]]):
        sel = (rows >= lo) & (rows <= hi)
        bands.append({"rows": [int(lo), int(hi)], "median_px": float(np.median(err[sel])),
                      "mean_dx": float(np.mean(res[v][sel, 0])), "mean_dy": float(np.mean(res[v][sel, 1]))})
    return {"points": int(len(pts)), "tracked": int(v.sum()), "median_px": float(np.median(err)),
            "p95_px": float(np.percentile(err, 95)), "mean_dx": float(np.mean(res[v, 0])),
            "mean_dy": float(np.mean(res[v, 1])), "bands": bands,
            # The band nearest the horizon is dominated by texture aliasing (the texture is minified
            # many times there), so the slip criterion applies to the rest of the sea.
            "pass": bool(np.median(err) < 0.5 and max(b["median_px"] for b in bands[1:]) < 0.3)}


def main(argv):
    plane_y = float(argv[3]) if len(argv) > 3 else 0.0
    _, ea = load(argv[1])
    _, eb = load(argv[2])
    rep = {f"eye_{'LR'[i]}": check(ea[i], eb[i], plane_y) for i in range(2)}
    rep["pass"] = all(r["pass"] for r in rep.values())
    print(json.dumps(rep, indent=1))
    return 0 if rep["pass"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
