"""Finds a shot's impact in a capture taken just after it, and measures it against the aim line.

    python impact_find.py <before.png> <after.png> <out.png> --line "<ev=vr.aim line ...>"
                          [--ray ox,oy,oz,dx,dy,dz --cam cx,cy,cz] [--null <null.png>]
                          [--decal] [--muzzle-px 60] [--json]

`--decal` treats `after` as a capture taken seconds after the shot and finds the mark the round left
(a darkening, exactly at the hit point) instead of the flash; `--null` should then span the same
interval. This is the reference: an explosion's flash is centred ~20 cm above the point hit.

`--ray` measures against the ray the round was really fired along (world, from `ev=vr.aim shot`),
projected with the line's camera placed at `--cam`, instead of the line's own origin and direction.

Captured with the gun out of frame and the drawn line off (Test-Aim.ps1), so what changes between
the two captures is the world: the impact, the tracer, whatever light the muzzle flash throws -- and,
for an explosion near the player, a radial blur the game lays over the WHOLE frame, which defeats
any local-contrast test. What survives all of it is the impact's flash, the brightest new light in
the frame, so:

  1. take the luminance GAIN of the capture after the shot over the one just before it;
  2. mask what changed between the two null captures (a blinking beacon, grass), the HUD, and a small
     disc at the muzzle;
  3. smooth at the scale of a flash (sigma 14 px), which favours it over point lights and over the
     tracer -- a thin streak lying ON the barrel line by construction, which would flatter the result;
  4. the peak is the impact, refined to the light-weighted centroid of its core.

The aim line comes from the DLL (`ev=vr.aim line`): its origin and vanishing point in normalised
device coordinates, projected with the camera the world was rendered with, plus that camera's
horizontal FOV and aspect. Pixels are converted to tangent coordinates (x = dot(p, right) / depth,
y = dot(p, up) / depth), where a straight 3D line is a straight line; the line and the camera span
a plane, and the miss is the angle between the ray to the impact and that plane. No depth is
needed: an impact on the shot's line is on the projected line at whatever distance it happens.

`along` is where the impact sits between the muzzle (0) and the vanishing point (1); an impact can
only be beyond the muzzle, so a negative value means the detector found something else.
"""
import json
import math
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def parse_line(text):
    def vec(name, n):
        m = re.search(name + r"=([-\d.,]+)", text)
        return [float(v) for v in m.group(1).split(",")[:n]]
    o = vec("ndc_origin", 3)
    v = vec("ndc_vanish", 3)
    return {"origin_ok": o[0] == 1, "origin": (o[1], o[2]), "vanish_ok": v[0] == 1, "vanish": (v[1], v[2]),
            "hfov": vec("hfov", 1)[0], "aspect": vec("aspect", 1)[0]}


def project_ray(line_text, ray, cam):
    """NDC of a world ray's origin and vanishing point, with the camera of an `ev=vr.aim line`.

    The line's camera basis, FOV and aspect are the frame's; `cam` is the camera's world position.
    Same arithmetic as `project` in vr_aim.cpp (game axes: right = forward x up).
    """
    def vec(name):
        return np.array([float(v) for v in re.search(name + r"=([-\d.,]+)", line_text).group(1).split(",")[:3]])
    f = vec("cam_fwd")
    f /= np.linalg.norm(f)
    u = vec("cam_up")
    u /= np.linalg.norm(u)
    r = np.cross(f, u)
    r /= np.linalg.norm(r)
    hfov = float(re.search(r"hfov=([-\d.]+)", line_text).group(1))
    aspect = float(re.search(r" aspect=([-\d.]+)", line_text).group(1))
    t = math.tan(hfov / 2)

    def ndc(rel):
        depth = float(rel @ f)
        if depth <= 1e-3:
            return False, (0.0, 0.0)
        return True, (float(rel @ r) / (t * depth), float(rel @ u) * aspect / (t * depth))
    o_ok, o = ndc(np.array(ray[:3]) - np.array(cam))
    v_ok, v = ndc(np.array(ray[3:]) * 1.0e4)
    return {"origin_ok": o_ok, "origin": o, "vanish_ok": v_ok, "vanish": v, "hfov": hfov, "aspect": aspect}


def to_pixel(ndc, w, h):
    return ((ndc[0] + 1) * 0.5 * w, (1 - ndc[1]) * 0.5 * h)


def to_tangent(px, py, w, h, hfov, aspect):
    t = math.tan(hfov / 2)
    xn = px / (w / 2) - 1
    yn = 1 - py / (h / 2)
    return (xn * t, yn * t / aspect)


def filt(mask, f):
    return np.asarray(Image.fromarray((mask * 255).astype(np.uint8)).filter(f)) > 127


def arg(a, name, default, cast=str):
    return cast(a[a.index(name) + 1]) if name in a else default


def main():
    a = sys.argv[1:]
    before, after, out = a[0], a[1], a[2]
    line = parse_line(arg(a, "--line", ""))
    if "--ray" in a:
        # The ray the round was really fired along (world), instead of the line's own origin and
        # direction, which were taken a moment before the shot.
        ray = [float(v) for v in arg(a, "--ray", "").split(",")]
        cam = [float(v) for v in arg(a, "--cam", "").split(",")]
        line = project_ray(arg(a, "--line", ""), ray, cam)
    muzzle_px = arg(a, "--muzzle-px", 60, float)
    A = np.asarray(Image.open(before).convert("RGB"), dtype=np.float32)
    B = np.asarray(Image.open(after).convert("RGB"), dtype=np.float32)
    h, w, _ = B.shape
    # The impact's flash: the brightest NEW light in the frame. Its saturated core survives what
    # else the shot does to the picture -- in particular the radial blur the game throws over the
    # whole frame for an explosion near the player, which changes every pixel and defeats any
    # local-contrast test. Smoothing at the scale of a flash (sigma 14 px) favours it over a point
    # light and over a tracer, which is a thin streak lying ON the barrel line by construction and
    # would flatter the result if it were counted.
    lum = np.array([0.299, 0.587, 0.114], dtype=np.float32)
    decal = "--decal" in a
    if decal:
        # `after` is a capture taken seconds later, when the flash, dust and blur are gone and what
        # is left is the mark the round made where it hit -- a small dark decal, exactly at the hit
        # point. An explosion's flash is centred some 20 cm ABOVE that point (measured: the flash
        # 1.5 deg off the fired ray where its decal is 0.28 deg off), so the decal is the reference.
        gain = np.clip(A @ lum - B @ lum, 0, 255)
    else:
        gain = np.clip(B @ lum - A @ lum, 0, 255)
    mask = np.ones(gain.shape, dtype=bool)
    # The drawn aim line, if it was left on: pure magenta.
    def magenta(img):
        return (img[:, :, 0] > 150) & (img[:, :, 2] > 150) & (img[:, :, 1] < 110)
    mask &= ~filt(magenta(A) | magenta(B), ImageFilter.MaxFilter(9))
    if "--null" in a:
        # Whatever changed between the two captures before the shot -- a blinking beacon, swaying
        # grass -- is the scene's own, and is masked out (dilated, since it moves).
        N = np.asarray(Image.open(arg(a, "--null", "")).convert("RGB"), dtype=np.float32)
        mask &= ~filt(np.abs(A - N).max(axis=2) > 20, ImageFilter.MaxFilter(15))
    # HUD and logo.
    mask[int(h * 0.80):, : int(w * 0.34)] = False
    mask[: int(h * 0.13), : int(w * 0.12)] = False
    ox, oy = to_pixel(line["origin"], w, h) if line["origin_ok"] else (w * 0.6, h * 0.8)
    yy, xx = np.mgrid[0:h, 0:w]
    mask &= (xx - ox) ** 2 + (yy - oy) ** 2 > muzzle_px ** 2
    sigma, core = (4, 12) if decal else (14, 30)
    smooth = np.asarray(Image.fromarray((gain * mask).astype(np.uint8)).filter(ImageFilter.GaussianBlur(sigma)),
                        dtype=np.float32)
    peak = float(smooth.max())
    result = {"found": False, "score": peak}
    # A mark is small and faint; below this the peak is the scene's own noise (measured: real marks
    # score 44-109; a round that landed in an earlier mark, too far to resolve, or on sand lit by
    # Io's day, 9-33, and 33 was stone shading). Better inconclusive than wrong.
    if peak >= (40.0 if decal else 20.0):
        py_, px_ = np.unravel_index(int(np.argmax(smooth)), smooth.shape)
        # Refine to the weighted centroid of the core.
        sel = ((xx - px_) ** 2 + (yy - py_) ** 2 < core ** 2) & mask
        wts = gain[sel]
        cx = float((xx[sel] * wts).sum() / max(wts.sum(), 1e-6))
        cy = float((yy[sel] * wts).sum() / max(wts.sum(), 1e-6))
        result.update({"found": True, "x": cx, "y": cy})
        if line["vanish_ok"] and not line["origin_ok"]:
            # A ray that starts at the camera (the flat game's own round, from the eye) projects to
            # a single point: the miss is simply the angle to its vanishing point.
            vx, vy = to_pixel(line["vanish"], w, h)
            hf, asp = line["hfov"], line["aspect"]
            p = np.array([*to_tangent(cx, cy, w, h, hf, asp), 1.0])
            v = np.array([*to_tangent(vx, vy, w, h, hf, asp), 1.0])
            miss_deg = math.degrees(math.acos(min(1.0, float(p @ v) / (np.linalg.norm(p) * np.linalg.norm(v)))))
            result.update({"miss_deg": miss_deg, "offset_px": (cx - vx, cy - vy), "vanish_px": (vx, vy)})
        elif line["vanish_ok"]:
            vx, vy = to_pixel(line["vanish"], w, h)
            hf, asp = line["hfov"], line["aspect"]
            p = np.array(to_tangent(cx, cy, w, h, hf, asp))
            o = np.array(to_tangent(ox, oy, w, h, hf, asp))
            v = np.array(to_tangent(vx, vy, w, h, hf, asp))
            dv = v - o
            along = float(np.dot(p - o, dv) / (dv @ dv + 1e-12))
            # The 3D line and the camera span a plane; the miss is the angle between the ray to the
            # impact and that plane, exactly.
            normal = np.cross(np.array([o[0], o[1], 1.0]), np.array([v[0], v[1], 1.0]))
            ray = np.array([p[0], p[1], 1.0])
            miss_deg = math.degrees(math.asin(min(1.0, abs(float(ray @ normal)) / (np.linalg.norm(ray) * np.linalg.norm(normal)))))
            # Which side of the line, in pixels: the impact minus the nearest point of the drawn line.
            # A flash that sits ABOVE the point the round hit (an explosion's core) reads as a
            # consistent negative y here, whatever the pose.
            seg = np.array([vx - ox, vy - oy])
            t = float(np.dot(np.array([cx - ox, cy - oy]), seg) / (seg @ seg + 1e-9))
            off = (cx - (ox + t * seg[0]), cy - (oy + t * seg[1]))
            result.update({"miss_deg": miss_deg, "along": along, "offset_px": off, "vanish_px": (vx, vy),
                           "origin_px": (ox, oy)})
    img = Image.open(after).convert("RGB")
    dr = ImageDraw.Draw(img)
    if line["vanish_ok"]:
        vx, vy = to_pixel(line["vanish"], w, h)
        if line["origin_ok"]:
            dr.line([ox, oy, vx, vy], fill=(0, 255, 255), width=1)
        dr.ellipse([vx - 6, vy - 6, vx + 6, vy + 6], outline=(0, 255, 255), width=2)
    if result["found"]:
        dr.ellipse([result["x"] - 14, result["y"] - 14, result["x"] + 14, result["y"] + 14], outline=(255, 255, 0), width=3)
    img.save(out)
    print(json.dumps(result) if "--json" in a else result)


if __name__ == "__main__":
    main()
