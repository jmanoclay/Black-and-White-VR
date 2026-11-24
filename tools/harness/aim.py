"""Mock-controller pose that points exactly through a pixel of a left-eye capture.

The controller is placed at the left eye, so its ray is the eye's ray through that pixel (what
the game would pick there). Head pose (yaw, pitch; no roll) must match the one used for the capture.

Usage: python tools/harness/aim.py <capture> <crop_x> <crop_y> <head_yaw> <head_pitch> [side]
Prints the stereo.req line, e.g. "ctrl r pose 12.3 -40.1 0 -0.031 0 0".
"""
import json
import math
import pathlib
import sys

CAPTURES = pathlib.Path(r"D:\Games\Black & White\bwvr\captures")


def ypr(yaw, pitch):
    """Columns (right, up, fwd) of the engine's basis_from_ypr(yaw, pitch, 0)."""
    y, p = math.radians(yaw), math.radians(pitch)
    yaw_b = [(math.cos(y), 0, -math.sin(y)), (0, 1, 0), (math.sin(y), 0, math.cos(y))]
    pitch_b = [(1, 0, 0), (0, math.cos(p), -math.sin(p)), (0, math.sin(p), math.cos(p))]

    def apply(b, v):
        return tuple(b[0][i] * v[0] + b[1][i] * v[1] + b[2][i] * v[2] for i in range(3))

    return [apply(yaw_b, c) for c in pitch_b]


def main(argv):
    name, cx, cy, head_yaw, head_pitch = argv[1], float(argv[2]), float(argv[3]), float(argv[4]), float(argv[5])
    side = argv[6] if len(argv) > 6 else "r"
    eye = json.loads((CAPTURES / f"{name}.json").read_text())["eyes"][0]
    rw, rh = eye["render"]
    x0, y0 = eye["crop"][:2]
    f = rw * 0.5 / math.tan(eye["fov"] * 0.5)
    rx, ry = cx + x0, cy + y0
    d_cam = ((rx - rw / 2) / f, (rh / 2 - ry) / f, 1.0)
    b = ypr(head_yaw, head_pitch)
    d = [b[0][i] * d_cam[0] + b[1][i] * d_cam[1] + b[2][i] * d_cam[2] for i in range(3)]
    n = math.sqrt(sum(c * c for c in d))
    d = [c / n for c in d]
    yaw = math.degrees(math.atan2(d[0], d[2]))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
    off = [-0.0308 * c for c in b[0]]  # left eye: 30.8 mm along the head's -right axis
    print(f"ctrl {side} pose {yaw:.4f} {pitch:.4f} 0 {off[0]:.4f} {off[1]:.4f} {off[2]:.4f}")


if __name__ == "__main__":
    main(sys.argv)
