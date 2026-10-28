"""What a REAL compositor shows each eye: disparity between the two halves of an OculusMirror capture.

The mock's compositor (eye_view.py) is our model of a runtime; this reads the runtime itself. Meta's
OculusMirror.exe with `--RectilinearBothEyes --SymmetricFov` draws both eyes side by side, each
undistorted over the SAME symmetric field (the eyes' FOV mirrored to its widest). With a shared field
the two halves are directly comparable: a point at the same angle in both eyes sits at the same pixel.

    mono    the halves must be identical: any shift between them IS the double vision, in degrees
            (the 2026-10-16 Quest 3 session: the submission read ~15 deg divergent per eye).
    stereo  far things nearly identical (a few hundredths of a degree), near things crossed (the gun at
            0.3-0.5 m: 7-12 deg), nothing divergent, no vertical shift anywhere.

usage: python mirror_eyes.py <capture.png> [--against ref.png] [--tan TX TY] [--split auto|half]
                             [--out DIR] [--json]
  --against  instead of left vs right, each eye against the same eye of a reference capture (the
          declaration probes: scale, rotation and shift of the picture, in degrees)
  --tan   the shared field's half-tangents (default 1.3764 1.4281: a Quest 3 through Link, the eyes'
          -54/+40/44/-55 deg mirrored). Only the conversion to degrees uses it.

Two independent measurements, so one cannot fool the other:
  - global: phase correlation of the whole halves (one dominant shift, sub-pixel, any size);
  - tiles:  40 px tiles of the left eye searched in the right eye over a wide window both ways
            (+-25 % of the width, +-10 % of the height), confident and unambiguous matches only.
"""

import json
import math
import os
import sys

import cv2
import numpy as np


def load_halves(path, split):
    img = cv2.imread(path, cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit("cannot read " + path)
    h, w = img.shape[:2]
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    if split == "auto":
        # The mirror letterboxes the pair inside its window: keep the rows and columns with content,
        # then split at the darkest column near the middle (the seam between the eyes).
        rows = np.where(gray.max(axis=1) > 8)[0]
        cols = np.where(gray.max(axis=0) > 8)[0]
        if rows.size and cols.size:
            img = img[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]
            gray = gray[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]
            h, w = img.shape[:2]
    left_end, right_start = w // 2, w // 2
    if split == "auto":
        # The seam: the run of dark columns near the middle, excluded from both eyes. No dark run:
        # the halves are exactly half each.
        lo = int(w * 0.45)
        band = gray[:, lo:int(w * 0.55)].astype(np.float32).mean(axis=0)
        dark = np.where(band < 8)[0]
        if dark.size:
            centre = dark[np.argmin(np.abs(dark - band.size / 2))]
            a = b = centre
            while a - 1 >= 0 and band[a - 1] < 8:
                a -= 1
            while b + 1 < band.size and band[b + 1] < 8:
                b += 1
            left_end, right_start = lo + a, lo + b + 1
    left = img[:, :left_end]
    right = img[:, right_start:]
    width = min(left.shape[1], right.shape[1])
    return left[:, :width], right[:, :width]


def to_deg(px, size, tan_half):
    """A pixel shift at the image centre, in degrees: the halves are linear in tangent."""
    ppt = size / (2.0 * tan_half)
    return math.degrees(math.atan(px / ppt))


def global_shift(l, r):
    gl = cv2.cvtColor(l, cv2.COLOR_BGR2GRAY).astype(np.float32)
    gr = cv2.cvtColor(r, cv2.COLOR_BGR2GRAY).astype(np.float32)
    win = cv2.createHanningWindow(gl.shape[::-1], cv2.CV_32F)
    (dx, dy), response = cv2.phaseCorrelate(gl, gr, win)
    # phaseCorrelate returns how far r is shifted relative to l; disparity is left minus right.
    return -dx, -dy, response


def tile_shifts(l, r, tile=40, step=24, min_score=0.85):
    gl = cv2.cvtColor(l, cv2.COLOR_BGR2GRAY).astype(np.float32)
    gr = cv2.cvtColor(r, cv2.COLOR_BGR2GRAY).astype(np.float32)
    h, w = gl.shape
    hs = int(w * 0.25)
    vs = int(h * 0.10)
    out = []
    for y in range(vs, h - tile - vs, step):
        for x in range(0, w - tile, step):
            t = gl[y:y + tile, x:x + tile]
            if t.std() < 8.0:
                continue
            x0 = max(0, x - hs)
            x1 = min(w, x + tile + hs)
            strip = gr[y - vs:y + tile + vs, x0:x1]
            if strip.shape[1] < tile + 4:
                continue
            res = cv2.matchTemplate(strip, t, cv2.TM_CCOEFF_NORMED)
            _, best, _, loc = cv2.minMaxLoc(res)
            if best < min_score:
                continue
            if loc[0] in (0, res.shape[1] - 1) or loc[1] in (0, res.shape[0] - 1):
                continue
            row = res[loc[1]].copy()
            row[max(0, loc[0] - 6):loc[0] + 7] = -1
            if row.size and row.max() > best - 0.04:
                continue

            def sub(a, i):
                if 0 < i < a.size - 1:
                    den = a[i - 1] - 2 * a[i] + a[i + 1]
                    return i + (0.5 * (a[i - 1] - a[i + 1]) / den if abs(den) > 1e-9 else 0.0)
                return float(i)

            xr = x0 + sub(res[loc[1]], loc[0])
            yr = y - vs + sub(res[:, loc[0]], loc[1])
            # Both axes left minus right, like the global shift.
            out.append((x + tile / 2.0, y + tile / 2.0, x - xr, y - yr, best))
    return np.array(out, dtype=np.float64) if out else np.zeros((0, 5))


def same_eye(a, b, tx, ty):
    """One eye in two captures: the similarity (scale, rotation, shift) taking a onto b, by SIFT.

    Used for the declaration probes: a declared pose turned 10 deg moves an honouring runtime's
    picture by 10 deg, a declared FOV 1.2x wider shrinks it to 0.833; an ignoring runtime shows the
    identity."""
    ga = cv2.cvtColor(a, cv2.COLOR_BGR2GRAY)
    gb = cv2.cvtColor(b, cv2.COLOR_BGR2GRAY)
    sift = cv2.SIFT_create(4000)
    ka, da = sift.detectAndCompute(ga, None)
    kb, db = sift.detectAndCompute(gb, None)
    if da is None or db is None:
        return {"matches": 0}
    pairs = cv2.BFMatcher().knnMatch(da, db, k=2)
    good = [m for m, n in (p for p in pairs if len(p) == 2) if m.distance < 0.75 * n.distance]
    if len(good) < 12:
        return {"matches": len(good)}
    pa = np.float32([ka[m.queryIdx].pt for m in good])
    pb = np.float32([kb[m.trainIdx].pt for m in good])
    M, inliers = cv2.estimateAffinePartial2D(pa, pb, method=cv2.RANSAC, ransacReprojThreshold=2.0)
    if M is None:
        return {"matches": len(good)}
    h, w = ga.shape
    scale = math.hypot(M[0, 0], M[1, 0])
    centre = np.array([w / 2.0, h / 2.0])
    moved = M[:, :2] @ centre + M[:, 2]
    return {"matches": int(inliers.sum()), "scale": round(scale, 4),
            "rot_deg": round(math.degrees(math.atan2(M[1, 0], M[0, 0])), 3),
            "h_deg": round(to_deg(moved[0] - centre[0], w, tx), 3),
            "v_deg": round(to_deg(moved[1] - centre[1], h, ty), 3)}


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    tx, ty = 1.3764, 1.4281
    split = "auto"
    out_dir = None
    as_json = False
    against = None
    i = 1
    while i < len(argv):
        if argv[i] == "--against":
            against = argv[i + 1]
            i += 2
        elif argv[i] == "--tan":
            tx, ty = float(argv[i + 1]), float(argv[i + 2])
            i += 3
        elif argv[i] == "--split":
            split = argv[i + 1]
            i += 2
        elif argv[i] == "--out":
            out_dir = argv[i + 1]
            i += 2
        elif argv[i] == "--json":
            as_json = True
            i += 1
        else:
            raise SystemExit("unknown argument " + argv[i])
    l, r = load_halves(path, split)
    h, w = l.shape[:2]
    if against:
        # Eye by eye against another capture (the reference first: how it moved INTO this one).
        bl, br = load_halves(against, split)
        result = {"capture": os.path.basename(path), "reference": os.path.basename(against),
                  "against": {"left": same_eye(bl, l, tx, ty), "right": same_eye(br, r, tx, ty)}}
        print(json.dumps(result) if as_json else json.dumps(result, indent=2))
        return 0
    gdx, gdy, resp = global_shift(l, r)
    t = tile_shifts(l, r)
    result = {
        "capture": os.path.basename(path),
        "eye_px": [w, h],
        "global": {"h_px": round(gdx, 2), "v_px": round(gdy, 2), "h_deg": round(to_deg(gdx, w, tx), 3),
                   "v_deg": round(to_deg(gdy, h, ty), 3), "response": round(resp, 3)},
        "tiles": int(len(t)),
    }
    if len(t):
        hdeg = np.array([to_deg(v, w, tx) for v in t[:, 2]])
        vdeg = np.array([to_deg(v, h, ty) for v in t[:, 3]])
        near = hdeg > 2.0
        far = ~near

        def stats(sel):
            if not sel.any():
                return None
            return {"tiles": int(sel.sum()), "h_median": round(float(np.median(hdeg[sel])), 3),
                    "h_p10": round(float(np.percentile(hdeg[sel], 10)), 3),
                    "h_p90": round(float(np.percentile(hdeg[sel], 90)), 3),
                    "v_median": round(float(np.median(vdeg[sel])), 3),
                    "v_abs_p95": round(float(np.percentile(np.abs(vdeg[sel]), 95)), 3),
                    "divergent": int((hdeg[sel] < -0.1).sum())}

        result["all"] = stats(np.ones(len(t), dtype=bool))
        result["far"] = stats(far)
        result["near"] = stats(near)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        stem = os.path.splitext(os.path.basename(path))[0]
        # An anaglyph of the pair: a single grey image where the eyes agree, red/cyan fringes where not.
        gl = cv2.cvtColor(l, cv2.COLOR_BGR2GRAY)
        gr = cv2.cvtColor(r, cv2.COLOR_BGR2GRAY)
        ana = np.dstack([gr, gr, gl])
        cv2.imwrite(os.path.join(out_dir, stem + "_anaglyph.png"), ana)
        cv2.imwrite(os.path.join(out_dir, stem + "_left.png"), l)
        cv2.imwrite(os.path.join(out_dir, stem + "_right.png"), r)
    if as_json:
        print(json.dumps(result))
    else:
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
