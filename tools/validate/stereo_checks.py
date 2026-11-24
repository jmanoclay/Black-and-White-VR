"""Automated checks on stereo captures written by the mod (bwvr/captures/<name>.json + _L/_R .bmp/.depth).

All geometry is done on the shared render pixel grid (both eyes use the same symmetric render
geometry; only the crop differs), with the engine's projection model:
    camera = p_world * R + T   (world_to_camera, row-vector, +Z forward)
    sx = render_w/2 + f * X/Z,  sy = render_h/2 - f * Y/Z
    depth buffer z = 1 - 1/Z

Usage:
  python tools/validate/stereo_checks.py single <capture>            coverage, probes, stereo geometry
  python tools/validate/stereo_checks.py identity <capture>          capture taken with ipd_scale 0
  python tools/validate/stereo_checks.py worldlock <capA> <capB>     same scene, different head pose
Each command prints a JSON report and writes <capture>_sheet.png next to the capture.
"""
import json
import pathlib
import sys

import cv2
import numpy as np

CAPTURES = pathlib.Path(r"D:\Games\Black & White\bwvr\captures")
MAGENTA = np.array([255, 0, 255])


class Eye:
    def __init__(self, meta, eye_meta, stem):
        self.m = eye_meta
        self.render_w, self.render_h = eye_meta["render"]
        self.cx0, self.cy0, self.cw, self.ch = eye_meta["crop"]
        self.f = self.render_w * 0.5 / np.tan(eye_meta["fov"] * 0.5)
        self.R = np.array([eye_meta["right"], eye_meta["up"], eye_meta["fwd"]])  # rows: camera axes (world)
        self.pos = np.array(eye_meta["pos"])
        self.w2c = np.array(eye_meta["world_to_camera"])
        self.img = cv2.cvtColor(cv2.imread(str(stem) + ".bmp"), cv2.COLOR_BGR2RGB)
        dpath = pathlib.Path(str(stem) + ".depth")
        self.z = None
        if eye_meta.get("depth") and dpath.exists():
            self.z = np.fromfile(dpath, dtype=np.float32).reshape(self.ch, self.cw)

    # crop pixel -> render-grid pixel
    def to_render(self, u, v):
        return u + self.cx0, v + self.cy0

    def project(self, p):
        """World point(s) (N,3) -> render pixel (N,2) and depth Z, by the engine's model."""
        p = np.atleast_2d(p)
        cam = p @ self.w2c[:3] + self.w2c[3]
        sx = self.render_w / 2 + self.f * cam[:, 0] / cam[:, 2]
        sy = self.render_h / 2 - self.f * cam[:, 1] / cam[:, 2]
        return np.stack([sx, sy], 1), cam[:, 2]

    def unproject(self, rx, ry, Z):
        """Render pixel + view depth -> world point."""
        tx = (rx - self.render_w / 2) / self.f
        ty = -(ry - self.render_h / 2) / self.f
        return self.pos + (self.R[0] * tx[:, None] + self.R[1] * ty[:, None] + self.R[2]) * Z[:, None]

    def depth_at(self, u, v):
        """View depth at crop pixels (from the 16-bit z-buffer), NaN where unavailable/infinite."""
        if self.z is None:
            return np.full(len(u), np.nan)
        z = self.z[np.clip(v.astype(int), 0, self.ch - 1), np.clip(u.astype(int), 0, self.cw - 1)]
        with np.errstate(divide="ignore"):
            Z = 1.0 / (1.0 - z)
        Z[z >= 0.99999] = np.inf
        return Z


def load(name):
    meta = json.loads((CAPTURES / f"{name}.json").read_text())
    eyes = [Eye(meta, meta["eyes"][i], CAPTURES / f"{name}_{'LR'[i]}") for i in range(2)]
    return meta, eyes


# ---------------------------------------------------------------- checks

def depth_from_pair(eye, other, pts):
    """View depth for crop points of `eye`, triangulated against the other eye of the same capture.

    Both eyes are parallel with identical pixel grids, so a point at depth Z appears shifted by
    f * bx / Z on the render grid, where bx is the baseline along eye's right axis. Points are
    located in `other` with sub-pixel optical flow seeded at the same render position.
    The D3D7 z-buffer can't be read back on this driver, so this is the depth source.
    """
    rx, ry = eye.to_render(pts[:, 0], pts[:, 1])
    guess = np.stack([rx - other.cx0, ry - other.cy0], 1)
    found, ok = refine(eye.img, other.img, pts, guess)
    dxr = rx - (found[:, 0] + other.cx0)
    dyr = ry - (found[:, 1] + other.cy0)
    bx = float(np.dot(other.pos - eye.pos, eye.R[0]))
    with np.errstate(divide="ignore", invalid="ignore"):
        Z = eye.f * bx / dxr
    valid = ok & (np.abs(dyr) < 1.0)
    Z = np.where(valid & (Z > 0) & (np.abs(dxr) > 0.2), Z, np.inf)
    return Z, valid, dxr, dyr


def check_coverage(eye):
    mask = np.all(np.abs(eye.img.astype(int) - MAGENTA) <= 2, axis=2)
    frac = float(mask.mean())
    return {"magenta_pixels": int(mask.sum()), "fraction": frac, "pass": frac < 1e-4}, mask


def check_probes(eye):
    errs, zerrs = [], []
    for pr in eye.m["probes"]:
        if not pr["vis"]:
            continue
        (sx, sy), Z = eye.project(np.array(pr["p"]))[0][0], eye.project(np.array(pr["p"]))[1][0]
        errs.append(max(abs(sx - pr["x"]), abs(sy - pr["y"])))
        zerrs.append(abs(Z - pr["depth"]) / max(1e-6, abs(Z)))
    if not errs:
        return {"count": 0, "pass": False, "note": "no visible probes"}
    return {"count": len(errs), "max_px_error": float(max(errs)), "max_rel_depth_error": float(max(zerrs)),
            "pass": max(errs) <= 1.5 and max(zerrs) < 1e-3}


def match_features(img_a, img_b, n=6000, ratio=0.75):
    orb = cv2.ORB_create(nfeatures=n, fastThreshold=10)
    ga, gb = cv2.cvtColor(img_a, cv2.COLOR_RGB2GRAY), cv2.cvtColor(img_b, cv2.COLOR_RGB2GRAY)
    ka, da = orb.detectAndCompute(ga, None)
    kb, db = orb.detectAndCompute(gb, None)
    if da is None or db is None:
        return np.zeros((0, 2)), np.zeros((0, 2))
    knn = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(da, db, k=2)
    good = [m for m, *rest in knn if rest and m.distance < ratio * rest[0].distance]
    pa = np.array([ka[m.queryIdx].pt for m in good]).reshape(-1, 2)
    pb = np.array([kb[m.trainIdx].pt for m in good]).reshape(-1, 2)
    return pa, pb


def check_stereo(meta, L, R):
    """Corners in L tracked into R: vertical disparity ~0, horizontal disparity never negative."""
    pl = track_points(L.img, 2000)
    if len(pl) < 30:
        return {"points": len(pl), "pass": False, "note": "too few corners"}, None
    Z, valid, dx, dy = depth_from_pair(L, R, pl)
    v = valid & (np.abs(dx) < 60)
    if v.sum() < 30:
        return {"points": int(len(pl)), "tracked": int(v.sum()), "pass": False}, None
    rep = {
        "points": int(len(pl)), "tracked": int(v.sum()),
        "baseline_world": float(np.linalg.norm(R.pos - L.pos)),
        "vertical_disparity_px": {"median_abs": float(np.median(np.abs(dy[v]))),
                                   "p95_abs": float(np.percentile(np.abs(dy[v]), 95))},
        "disparity_px": {"min": float(np.min(dx[v])), "p1": float(np.percentile(dx[v], 1)),
                         "median": float(np.median(dx[v])), "max": float(np.max(dx[v]))},
        "negative_disparity_fraction": float(np.mean(dx[v] < -0.5)),
        "depth_range_world": [float(np.nanmin(np.where(np.isfinite(Z[v]), Z[v], np.nan))),
                              float(np.nanmax(np.where(np.isfinite(Z[v]), Z[v], np.nan)))],
    }
    rep["pass"] = bool(rep["vertical_disparity_px"]["median_abs"] < 0.2 and rep["negative_disparity_fraction"] < 0.01)
    return rep, (pl[v], None, dx[v], dy[v])

def overlap(L, R):
    """Render-grid overlap of the two crops, as (L slice, R slice) in crop pixels."""
    x0, x1 = max(L.cx0, R.cx0), min(L.cx0 + L.cw, R.cx0 + R.cw)
    y0, y1 = max(L.cy0, R.cy0), min(L.cy0 + L.ch, R.cy0 + R.ch)
    ls = (slice(y0 - L.cy0, y1 - L.cy0), slice(x0 - L.cx0, x1 - L.cx0))
    rs = (slice(y0 - R.cy0, y1 - R.cy0), slice(x0 - R.cx0, x1 - R.cx0))
    return ls, rs


def check_identity(meta, L, R):
    ls, rs = overlap(L, R)
    a, b = L.img[ls].astype(int), R.img[rs].astype(int)
    diff = np.abs(a - b).max(axis=2)
    mse = float(np.mean((a - b) ** 2))
    psnr = 99.0 if mse == 0 else float(10 * np.log10(255 ** 2 / mse))
    rep = {"ipd_scale": meta["ipd_scale"], "overlap_px": int(diff.size), "max_abs_diff": int(diff.max()),
           "fraction_diff_gt_8": float(np.mean(diff > 8)), "psnr_db": psnr,
           "pass": bool(np.mean(diff > 8) < 1e-4)}
    return rep, diff


def refine(img_a, img_b, pts_a, guess_b):
    """Sub-pixel positions in B of points in A (pyramidal Lucas-Kanade seeded with a guess)."""
    ga = cv2.cvtColor(img_a, cv2.COLOR_RGB2GRAY)
    gb = cv2.cvtColor(img_b, cv2.COLOR_RGB2GRAY)
    # Eye crops can differ by a pixel; LK needs equal sizes, so pad both (top-left anchored).
    h, w = max(ga.shape[0], gb.shape[0]), max(ga.shape[1], gb.shape[1])
    ga = cv2.copyMakeBorder(ga, 0, h - ga.shape[0], 0, w - ga.shape[1], cv2.BORDER_CONSTANT)
    gb = cv2.copyMakeBorder(gb, 0, h - gb.shape[0], 0, w - gb.shape[1], cv2.BORDER_CONSTANT)
    p0 = pts_a.astype(np.float32).reshape(-1, 1, 2)
    p1 = guess_b.astype(np.float32).reshape(-1, 1, 2).copy()
    p1, st, err = cv2.calcOpticalFlowPyrLK(ga, gb, p0, p1, winSize=(21, 21), maxLevel=3,
                                           flags=cv2.OPTFLOW_USE_INITIAL_FLOW,
                                           criteria=(cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 50, 0.01))
    return p1.reshape(-1, 2), st.reshape(-1).astype(bool)


def track_points(img, n=1500):
    g = cv2.cvtColor(img, cv2.COLOR_RGB2GRAY)
    p = cv2.goodFeaturesToTrack(g, n, 0.01, 12)
    return np.zeros((0, 2)) if p is None else p.reshape(-1, 2)


def intrinsics(e):
    # render pixel = K * camera direction (y flipped: screen y grows downward)
    return np.array([[e.f, 0, e.render_w / 2], [0, -e.f, e.render_h / 2], [0, 0, 1.0]])


def rotation_homography(A, B):
    """Crop-pixel homography A -> B induced by the cameras' rotation (exact at infinity)."""
    to_render_a = np.array([[1, 0, A.cx0], [0, 1, A.cy0], [0, 0, 1.0]])
    to_crop_b = np.array([[1, 0, -B.cx0], [0, 1, -B.cy0], [0, 0, 1.0]])
    return to_crop_b @ intrinsics(B) @ B.R @ A.R.T @ np.linalg.inv(intrinsics(A)) @ to_render_a


def apply_h(H, pts):
    p = np.c_[pts, np.ones(len(pts))] @ H.T
    return p[:, :2] / p[:, 2:3]


def check_worldlock(A, A_other, B):
    """Same eye, two head poses: points must move exactly as the pose change predicts.

    Corners in A are lifted to 3D with stereo-triangulated depth, projected into B with B's
    camera, then located in B with sub-pixel optical flow seeded at that prediction. The residual
    (located - predicted) is the world-lock error. B is first warped into A's view by the
    rotation-only homography, so the tracker sees near-static patches even under roll.
    """
    pa = track_points(A.img)
    if len(pa) < 30:
        return {"points": len(pa), "pass": False, "note": "too few corners"}
    ax, ay = A.to_render(pa[:, 0], pa[:, 1])
    Z, zok, _, _ = depth_from_pair(A, A_other, pa)
    pa, ax, ay, Z = pa[zok], ax[zok], ay[zok], Z[zok]
    Z = np.where(np.isfinite(Z), Z, 1e7)  # no measurable disparity: effectively infinite
    pred, zb = B.project(A.unproject(ax, ay, Z))
    guess = pred - np.array([B.cx0, B.cy0])
    inside = (zb > 0) & (guess[:, 0] > 12) & (guess[:, 1] > 12) & (guess[:, 0] < B.cw - 12) & (guess[:, 1] < B.ch - 12)
    pa, guess, ax, ay = pa[inside], guess[inside], ax[inside], ay[inside]
    H = rotation_homography(A, B)  # crop A -> crop B, for points at infinity
    warped = cv2.warpPerspective(B.img, H, (A.cw, A.ch), flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP)
    Hinv = np.linalg.inv(H)
    found_w, ok = refine(A.img, warped, pa, apply_h(Hinv, guess))
    found = apply_h(H, found_w)
    res = found - guess
    errn = np.hypot(res[:, 0], res[:, 1])
    inl = ok & (errn < 6)
    moved = np.hypot(guess[:, 0] + B.cx0 - ax, guess[:, 1] + B.cy0 - ay)
    if inl.sum() < 20:
        return {"points": int(len(pa)), "inliers": int(inl.sum()), "pass": False}
    mean = res[inl].mean(0)
    return {"points": int(len(pa)), "inliers": int(inl.sum()),
            "predicted_motion_px_median": float(np.median(moved[inl])),
            "residual_px_median": float(np.median(errn[inl])),
            "residual_px_p90": float(np.percentile(errn[inl], 90)),
            "residual_mean_vector_px": [float(mean[0]), float(mean[1])],
            "pass": bool(inl.mean() > 0.6 and np.median(errn[inl]) < 0.5 and np.hypot(*mean) < 0.3)}

# ---------------------------------------------------------------- contact sheet

def anaglyph(L, R):
    ls, rs = overlap(L, R)
    gl = cv2.cvtColor(L.img[ls], cv2.COLOR_RGB2GRAY)
    gr = cv2.cvtColor(R.img[rs], cv2.COLOR_RGB2GRAY)
    return np.dstack([gl, gr, gr])


def sheet(path, tiles, width=1800):
    rows = []
    for row in tiles:
        h = 600
        imgs = []
        for t in row:
            if t.ndim == 2:
                t = cv2.cvtColor(t, cv2.COLOR_GRAY2RGB)
            s = h / t.shape[0]
            imgs.append(cv2.resize(t, (int(t.shape[1] * s), h), interpolation=cv2.INTER_AREA))
        r = np.concatenate(imgs, 1)
        rows.append(cv2.resize(r, (width, int(r.shape[0] * width / r.shape[1]))))
    out = np.concatenate(rows, 0)
    cv2.imwrite(str(path), cv2.cvtColor(out, cv2.COLOR_RGB2BGR))
    return path


def disparity_plot(L, feats):
    img = (L.img * 0.5).astype(np.uint8).copy()
    if feats is None:
        return img
    pl, _, dx, dy = feats
    for (u, v), d, e in zip(pl, dx, dy):
        bad = abs(e) > 1.0 or d < -1.0
        color = (255, 40, 40) if bad else tuple(int(c) for c in cv2.applyColorMap(
            np.uint8([[min(255, max(0, d * 8))]]), cv2.COLORMAP_TURBO)[0, 0][::-1])
        cv2.circle(img, (int(u), int(v)), 3, color, -1)
    return img


def main(argv):
    cmd = argv[0]
    if cmd == "single":
        name = argv[1]
        meta, (L, R) = load(name)
        cov = [check_coverage(e) for e in (L, R)]
        stereo, feats = check_stereo(meta, L, R)
        report = {"capture": name, "coverage": [c[0] for c in cov], "probes": [check_probes(L), check_probes(R)],
                  "stereo": stereo}
        report["pass"] = all(c[0]["pass"] for c in cov) and all(p["pass"] for p in report["probes"]) and stereo["pass"]
        masks = [np.where(c[1][..., None], np.uint8([255, 0, 255]), e.img // 3).astype(np.uint8) for c, e in zip(cov, (L, R))]
        report["sheet"] = str(sheet(CAPTURES / f"{name}_sheet.png",
                                    [[L.img, R.img], [anaglyph(L, R), disparity_plot(L, feats)], masks]))
    elif cmd == "identity":
        name = argv[1]
        meta, (L, R) = load(name)
        rep, diff = check_identity(meta, L, R)
        report = {"capture": name, "identity": rep, "pass": rep["pass"]}
        heat = cv2.applyColorMap(np.clip(diff * 8, 0, 255).astype(np.uint8), cv2.COLORMAP_INFERNO)[..., ::-1]
        report["sheet"] = str(sheet(CAPTURES / f"{name}_sheet.png", [[L.img, R.img], [anaglyph(L, R), heat]]))
    elif cmd == "worldlock":
        (ma, ea), (mb, eb) = load(argv[1]), load(argv[2])
        reps = [check_worldlock(ea[i], ea[1 - i], eb[i]) for i in range(2)]
        report = {"captures": argv[1:3], "worldlock": reps, "pass": all(r["pass"] for r in reps)}
        report["sheet"] = str(sheet(CAPTURES / f"{argv[2]}_worldlock_sheet.png", [[ea[0].img, eb[0].img]]))
    else:
        print(__doc__)
        return 2
    print(json.dumps(report, indent=2, default=lambda o: o.item() if hasattr(o, "item") else str(o)))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
