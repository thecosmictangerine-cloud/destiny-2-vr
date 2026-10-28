"""What the headset shows, measured: the mock compositor's per-eye images, analysed.

The mock (mockxr/mock_runtime.cpp, "The compositor, emulated") writes, on request,
SVR_seen_<tag>_eye0.bmp / _eye1.bmp -- each eye's whole field of view, linear in tan like a real
eye buffer, pixels no layer reaches painted dark blue (0, 0, 70) -- and SVR_seen_<tag>.txt with the
geometry. This turns those into numbers and pictures that answer, without a headset:

  coverage   how much of each eye's field holds an image, and the uncovered band at each edge in
             degrees (the black bands of the first sessions).
  density    source pixels per panel pixel, horizontally and vertically. Their ratio is the
             stretch: 1.00 means square pixels; the old layer read 1.65.
  disparity  where each eye sees the same thing, resampled to one shared angular grid. Positive
             (crossed) = nearer than infinity, which is what stereo should give the gun; zero =
             infinitely far, all that mono can give; NEGATIVE = beyond infinity, the eyes would
             have to diverge: impossible to fuse, a hard error. Vertical disparity must be ~0 --
             a fraction of a degree of it is already double vision.

Usage:
  eye_view.py <dir> <tag> [--out DIR] [--ipd M]   (default: the IPD the mock printed)
  eye_view.py diff <dir> <tagA> <tagB> [--out DIR]      # the swim test: two world-fixed composites
  eye_view.py seq <dir> <tag> <n> [--out DIR]           # a sequence: which eye got a fresh image when
  eye_view.py fidelity <dir> <tagRef> <tagTest> [--out DIR]   # same scene two ways: must be identity

Writes <tag>_eyes.png (side by side), <tag>_anaglyph.png (red = left, cyan = right),
<tag>_disp.png (disparity map, degrees) and prints a summary; `--json` for machine use.
"""
import json
import math
import os
import re
import sys

import cv2
import numpy as np

NO_IMAGE = np.array([0, 0, 70], dtype=np.uint8)   # RGB, as the mock paints it


def parse_info(path):
    info = {"eyes": {}, "views": {}, "coverage": {}, "layers": []}
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            if w[0] == "eye":
                i = int(w[1])
                info["eyes"][i] = {
                    "fov": [float(x) for x in w[3:7]],
                    "q": [float(x) for x in w[w.index("q") + 1: w.index("q") + 5]],
                    "p": [float(x) for x in w[w.index("p") + 1: w.index("p") + 4]],
                }
            elif w[0] == "view":
                i = int(w[1])
                rect = [int(x) for x in w[w.index("rect") + 1: w.index("rect") + 5]]
                swap = w[w.index("swap") + 2]
                sw, sh = (int(x) for x in swap.split("x"))
                info["views"][i] = {
                    "fov": [float(x) for x in w[3:7]],
                    "q": [float(x) for x in w[w.index("q") + 1: w.index("q") + 5]],
                    "p": [float(x) for x in w[w.index("p") + 1: w.index("p") + 4]],
                    "rect": rect,
                    "swap_ptr": w[w.index("swap") + 1],
                    "swap": (sw, sh),
                    "image": int(w[w.index("image") + 1]),
                    "releases": int(w[w.index("releases") + 1]),
                }
            elif w[0] == "coverage":
                info["coverage"][int(w[1])] = float(w[2])
            elif w[0] == "frame":
                info["frame"] = int(w[1])
            elif w[0] == "panel":
                info["panel"] = (int(w[1]), int(w[2]))
                info["ipd"] = float(w[4])
            elif w[0] == "layer":
                info["layers"].append(int(w[3]))
    return info


def load_bmp(path):
    img = cv2.imread(path, cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"cannot read {path}")
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB)


def tan_deg(d):
    return math.tan(math.radians(d))


def no_image_mask(img):
    return np.all(img == NO_IMAGE, axis=2)


def band_report(img, fov):
    """Uncovered degrees at each edge, along the eye's centre row and column (the optical axis)."""
    h, w, _ = img.shape
    l, r, u, d = (tan_deg(x) for x in fov)
    miss = no_image_mask(img)
    # The optical axis: tan 0 in both directions.
    cx = int(round((0 - l) / (r - l) * w - 0.5))
    cy = int(round((u - 0) / (u - d) * h - 0.5))
    cx = min(max(cx, 0), w - 1)
    cy = min(max(cy, 0), h - 1)
    row = miss[cy, :]
    col = miss[:, cx]

    def first_covered(a):
        idx = np.nonzero(~a)[0]
        return (idx[0], idx[-1]) if idx.size else (None, None)

    x0, x1 = first_covered(row)
    y0, y1 = first_covered(col)

    def ang_x(px):
        return math.degrees(math.atan(l + (px + 0.5) / w * (r - l)))

    def ang_y(px):
        return math.degrees(math.atan(u - (px + 0.5) / h * (u - d)))

    if x0 is None or y0 is None:
        return {"left": None, "right": None, "top": None, "bottom": None}
    return {
        # uncovered angle at each edge = the eye's edge minus where the image starts
        "left": round(ang_x(x0) - fov[0], 2),
        "right": round(fov[1] - ang_x(x1), 2),
        "top": round(fov[2] - ang_y(y0), 2),
        "bottom": round(ang_y(y1) - fov[3], 2),
    }


def black_bands(img, fov):
    """Black INSIDE the image, at each edge, in degrees: rows (columns) whose every pixel is near-black,
    counted inwards from each edge of the eye. Coverage only says a layer reached a pixel; a 16:10
    world letterboxed inside an eye-shaped image covers 100 % and still shows a black band -- the
    defect this exists to catch. A real scene is never black across a whole row of the eye."""
    h, w, _ = img.shape
    l, r, u, d = (tan_deg(x) for x in fov)
    dark = np.all(img < 6, axis=2) | no_image_mask(img)
    row_dark = dark.mean(axis=1) > 0.995
    col_dark = dark.mean(axis=0) > 0.995

    def run(a):
        n = 0
        for v in a:
            if not v:
                break
            n += 1
        return n

    t, b = run(row_dark), run(row_dark[::-1])
    lf, rt = run(col_dark), run(col_dark[::-1])

    def ang_y(px):
        return math.degrees(math.atan(u - px / h * (u - d)))

    def ang_x(px):
        return math.degrees(math.atan(l + px / w * (r - l)))

    return {"top": round(fov[2] - ang_y(t), 2), "bottom": round(ang_y(h - b) - fov[3], 2),
            "left": round(ang_x(lf) - fov[0], 2), "right": round(fov[1] - ang_x(w - rt), 2)}


def density(view, eye, eye_px):
    """Source pixels per panel pixel along each axis, for the projection view an eye shows."""
    vl, vr, vu, vd = (tan_deg(x) for x in view["fov"])
    el, er, eu, ed = (tan_deg(x) for x in eye["fov"])
    rw, rh = view["rect"][2], view["rect"][3]
    src_x = rw / (vr - vl)     # source px per tan unit
    src_y = rh / (vu - vd)
    pan_x = eye_px[0] / (er - el)
    pan_y = eye_px[1] / (eu - ed)
    return {"h": round(src_x / pan_x, 3), "v": round(src_y / pan_y, 3), "stretch": round((src_x / src_y), 3)}


def common_grid(info, imgs):
    """Both eyes resampled onto the angular region they share, at the eye images' density."""
    e0, e1 = info["eyes"][0], info["eyes"][1]
    l = max(tan_deg(e0["fov"][0]), tan_deg(e1["fov"][0]))
    r = min(tan_deg(e0["fov"][1]), tan_deg(e1["fov"][1]))
    u = min(tan_deg(e0["fov"][2]), tan_deg(e1["fov"][2]))
    d = max(tan_deg(e0["fov"][3]), tan_deg(e1["fov"][3]))
    h0, w0, _ = imgs[0].shape
    ppt = w0 / (tan_deg(e0["fov"][1]) - tan_deg(e0["fov"][0]))   # px per tan unit
    gw = int((r - l) * ppt)
    gh = int((u - d) * ppt)
    out = []
    for i in (0, 1):
        e = info["eyes"][i]
        el, er, eu, ed = (tan_deg(x) for x in e["fov"])
        hh, ww, _ = imgs[i].shape
        xs = l + (np.arange(gw) + 0.5) / gw * (r - l)
        ys = u - (np.arange(gh) + 0.5) / gh * (u - d)
        mx = ((xs - el) / (er - el) * ww - 0.5).astype(np.float32)
        my = ((eu - ys) / (eu - ed) * hh - 0.5).astype(np.float32)
        mapx = np.tile(mx[None, :], (gh, 1))
        mapy = np.tile(my[:, None], (1, gw))
        out.append(cv2.remap(imgs[i], mapx, mapy, cv2.INTER_LINEAR))
    return out, ppt, (l, r, u, d)


def disparity(grid, ppt):
    """Horizontal disparity (SGBM, signed) and vertical disparity (phase correlation on tiles)."""
    g0 = cv2.cvtColor(grid[0], cv2.COLOR_RGB2GRAY)
    g1 = cv2.cvtColor(grid[1], cv2.COLOR_RGB2GRAY)
    valid = ~(no_image_mask(grid[0]) | no_image_mask(grid[1]))
    # Left image = eye 0. A point nearer than infinity sits further RIGHT in the left eye's image:
    # x_left - x_right = ipd / z > 0. minDisparity below zero so a divergent error is SEEN, not clipped.
    min_d = -32
    num_d = 16 * 14   # up to ~190 px: a hand at 25 cm at full resolution
    sgbm = cv2.StereoSGBM_create(minDisparity=min_d, numDisparities=num_d, blockSize=7,
                                 P1=8 * 7 * 7, P2=32 * 7 * 7, uniquenessRatio=12,
                                 speckleWindowSize=80, speckleRange=2, disp12MaxDiff=2,
                                 mode=cv2.STEREO_SGBM_MODE_SGBM_3WAY)
    # SGBM leaves its first (min + num) columns unmatched: pad on the left and crop after, or the
    # left quarter of the shared field would never be measured.
    pad = num_d + abs(min_d)
    p0 = cv2.copyMakeBorder(g0, 0, 0, pad, 0, cv2.BORDER_REPLICATE)
    p1 = cv2.copyMakeBorder(g1, 0, 0, pad, 0, cv2.BORDER_REPLICATE)
    d = sgbm.compute(p0, p1).astype(np.float32)[:, pad:] / 16.0
    good = (d > min_d) & valid
    # Texture: SGBM reports something everywhere it can; keep only where there is detail to match.
    lap = cv2.Laplacian(cv2.GaussianBlur(g0, (3, 3), 0), cv2.CV_32F)
    texture = cv2.blur(np.abs(lap), (9, 9)) > 4.0
    good &= texture
    # Same-pixel agreement: the left-right difference where disparity is ~0 is the cheapest proof of mono.
    deg = np.degrees(np.arctan(d / ppt))   # small-angle accurate enough; d in px of the shared grid

    # Vertical disparity by dense optical flow (DIS). Tile phase correlation was tried first and
    # rejected: OpenCV's sub-pixel peak is biased towards whole pixels (a known 2.00 px shift read
    # 1.1 to 1.9), and ECC refinement from it stalled at 1.8; DIS reads 1.998 on the same pair.
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_ULTRAFAST)
    dis.setFinestScale(0)
    dis.setPatchSize(8)
    dis.setPatchStride(4)
    dis.setVariationalRefinementIterations(10)
    flow = dis.calc(g0, g1, None)
    sel = valid & texture
    vs = flow[..., 1][sel]
    hs = flow[..., 0][sel]
    ws = np.ones_like(vs)
    return d, deg, good, hs, vs, ws


def tile_match(grid, ppt, tile=40, step=20, search=(-16, 240), vsearch=8, min_score=0.85, near_deg=2.0):
    """Disparity of whole tiles by normalised cross-correlation, horizontal AND vertical.

    SGBM is a pixel matcher with a smoothness prior: on a dark, smooth gun 10 degrees nearer than the
    ground behind it, it matches the background or nothing (it read 1.1 deg for a gun the anaglyph
    shows at ~12). DIS flow has the same limit and turns occlusion edges into spurious vertical
    motion. A 40 px tile carries enough of the gun's own texture to be found unambiguously, so each
    textured tile of the left eye is searched for in the right eye over -16..+240 px horizontally and
    +-8 vertically; only confident, unambiguous matches count. Near = nearer than ~1.8 m (2 deg at
    62 mm); far = everything else, where vertical disparity must be zero.
    """
    g0 = cv2.cvtColor(grid[0], cv2.COLOR_RGB2GRAY).astype(np.float32)
    g1 = cv2.cvtColor(grid[1], cv2.COLOR_RGB2GRAY).astype(np.float32)
    valid = ~(no_image_mask(grid[0]) | no_image_mask(grid[1]))
    h, w = g0.shape
    lo, hi = search
    rows = []
    for y in range(vsearch, h - tile - vsearch, step):
        for x in range(0, w - tile, step):
            if not valid[y:y + tile, x:x + tile].all():
                continue
            t = g0[y:y + tile, x:x + tile]
            if t.std() < 6.0:
                continue
            # the right-eye strip: x_right = x_left - d, d in [lo, hi]
            xs0 = max(0, x - hi)
            xs1 = min(w, x + tile - lo)
            if xs1 - xs0 < tile + 4:
                continue
            strip = g1[y - vsearch:y + tile + vsearch, xs0:xs1]
            if not valid[y - vsearch:y + tile + vsearch, xs0:xs1].all():
                continue
            r = cv2.matchTemplate(strip, t, cv2.TM_CCOEFF_NORMED)
            _, best, _, loc = cv2.minMaxLoc(r)
            if best < min_score:
                continue
            # A peak on the edge of the search window is not a peak: the optimum lies outside it, and
            # what is reported is the clipped edge (it read exactly +-8 px on smooth gloves, a "1.1 deg
            # vertical disparity" that was only the window). Discarded, both axes.
            if loc[1] == 0 or loc[1] == r.shape[0] - 1 or loc[0] == 0 or loc[0] == r.shape[1] - 1:
                continue
            # ambiguity: the best match elsewhere along the row (outside +-6 px) must be clearly worse
            row = r[loc[1]].copy()
            row[max(0, loc[0] - 6):loc[0] + 7] = -1
            if row.size and row.max() > best - 0.04:
                continue
            # sub-pixel by a parabola through the peak, both axes
            def sub(a, i):
                if 0 < i < a.size - 1:
                    den = a[i - 1] - 2 * a[i] + a[i + 1]
                    return i + (0.5 * (a[i - 1] - a[i + 1]) / den if abs(den) > 1e-9 else 0.0)
                return float(i)
            fx = sub(r[loc[1]], loc[0])
            fy = sub(r[:, loc[0]], loc[1])
            xr = xs0 + fx
            yr = y - vsearch + fy
            dx = x - xr
            dy = yr - y
            rows.append((x + tile / 2, y + tile / 2, dx, dy, best))
    if not rows:
        return {"tiles": 0}
    a = np.array(rows, dtype=np.float64)
    hdeg = np.degrees(np.arctan(a[:, 2] / ppt))
    vdeg = np.degrees(np.arctan(a[:, 3] / ppt))
    near = hdeg > near_deg
    far = ~near

    # Vertical disparity is judged on the confident matches only: a smooth dark glove matches almost
    # as well a pixel up or down, and a "vertical disparity" read there is the matcher's, not the eyes'.
    confident = a[:, 4] >= 0.92

    def stats(sel):
        if not sel.any():
            return None
        vsel = sel & confident
        if not vsel.any():
            vsel = sel
        return {"tiles": int(sel.sum()),
                "h_median": round(float(np.median(hdeg[sel])), 3),
                "h_p10": round(float(np.percentile(hdeg[sel], 10)), 3),
                "h_p90": round(float(np.percentile(hdeg[sel], 90)), 3),
                "v_tiles": int(vsel.sum()),
                "v_median": round(float(np.median(vdeg[vsel])), 3),
                "v_abs_p95": round(float(np.percentile(np.abs(vdeg[vsel]), 95)), 3),
                "divergent": int((hdeg[sel] < -0.1).sum())}
    return {"tiles": int(len(a)), "near": stats(near), "far": stats(far),
            "points": [(round(p[0], 1), round(p[1], 1), round(float(hd), 3), round(float(vd), 3))
                       for p, hd, vd in zip(a, hdeg, vdeg)]}


def near_objects_vertical(grid, ppt, points, near_deg=2.0):
    """Vertical disparity of the near objects, per side (left third: the left hand; right half: the
    gun hand), as the MEDIAN over their tiles. A single tile on a 30 cm object cannot be trusted
    vertically -- the eyes see it 12 degrees apart, edges occlude, slanted surfaces shear, and a smooth
    glove matches a pixel up or down as well -- but those errors scatter both ways, while anything that
    really displaces an object vertically between the eyes moves all its tiles together. Two other
    estimators were tried and dropped: a whole-region template mixes in the background's disparity,
    and dark-pixel row profiles are dominated by the game's vignette, identical in both eyes."""
    w = grid[0].shape[1]
    near = [p for p in points if p[2] > near_deg]
    out = []
    for side, sel in (("left", [p for p in near if p[0] < w / 3]), ("right", [p for p in near if p[0] >= w / 2])):
        if len(sel) < 6:
            continue
        v = np.sort(np.array([p[3] for p in sel]))
        k = len(v) // 5
        out.append({"side": side, "tiles": len(sel), "v_median": round(float(np.median(v)), 3),
                    "v_trimmed_mean": round(float(v[k:len(v) - k].mean()), 3)})
    return out


def colorize(deg, good, lo=-1.0, hi=6.0):
    t = np.clip((deg - lo) / (hi - lo), 0, 1)
    rgb = cv2.applyColorMap((t * 255).astype(np.uint8), cv2.COLORMAP_TURBO)
    rgb = cv2.cvtColor(rgb, cv2.COLOR_BGR2RGB)
    rgb[~good] = (rgb[~good] * 0.15).astype(np.uint8)
    # divergent (< -0.1 deg) in magenta, unmistakable
    neg = good & (deg < -0.1)
    rgb[neg] = (255, 0, 255)
    return rgb


def put(img, text, y, scale=0.7):
    cv2.putText(img, text, (10, y), cv2.FONT_HERSHEY_SIMPLEX, scale, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(img, text, (10, y), cv2.FONT_HERSHEY_SIMPLEX, scale, (255, 255, 255), 1, cv2.LINE_AA)


def analyse(folder, tag, out, ipd, as_json):
    info = parse_info(os.path.join(folder, f"SVR_seen_{tag}.txt"))
    if ipd is None:
        ipd = info.get("ipd", 0.064)   # the mock prints the IPD it renders the eyes with
    imgs = [load_bmp(os.path.join(folder, f"SVR_seen_{tag}_eye{i}.bmp")) for i in (0, 1)]
    os.makedirs(out, exist_ok=True)
    summary = {"tag": tag, "frame": info.get("frame"), "layers": info["layers"]}
    eye_px = {}
    for i in (0, 1):
        h, w, _ = imgs[i].shape
        eye_px[i] = (w, h)
        e = info["eyes"][i]
        s = {"coverage": round(float(info["coverage"].get(i, 0.0)), 4), "bands_deg": band_report(imgs[i], e["fov"]),
             "black_deg": black_bands(imgs[i], e["fov"])}
        v = info["views"].get(i)
        if v:
            # Against the PANEL, not the composite: the composite is half size unless asked for in full.
            # (A capture from before the mock printed its panel: assume half size.)
            s["density"] = density(v, e, info.get("panel", (2 * w, 2 * h)))
            s["declared_fov"] = v["fov"]
            s["image"] = v["image"]
            s["swap"] = v["swap_ptr"]
            s["releases"] = v["releases"]
            # declared pose vs where the eye is: the offset any stereo image must carry
            s["declared_p"] = v["p"]
        s["eye_p"] = e["p"]
        summary[f"eye{i}"] = s
    v0, v1 = info["views"].get(0), info["views"].get(1)
    if v0 and v1:
        dp = np.array(v1["p"]) - np.array(v0["p"])
        summary["declared_baseline_m"] = round(float(np.linalg.norm(dp)), 4)
        summary["same_image"] = (v0["swap_ptr"] == v1["swap_ptr"] and v0["image"] == v1["image"]
                                 and v0["rect"] == v1["rect"])

    grid, ppt, extent = common_grid(info, imgs)
    d, deg, good, hs, vs, ws = disparity(grid, ppt)
    gd = deg[good]
    summary["shared_fov_deg"] = [round(math.degrees(math.atan(x)), 1) for x in extent]
    summary["px_per_deg_centre"] = round(ppt * math.pi / 180.0, 2)
    if gd.size:
        summary["h_disparity_deg"] = {
            "matched_fraction": round(float(good.mean()), 3),
            "p1": round(float(np.percentile(gd, 1)), 3),
            "p5": round(float(np.percentile(gd, 5)), 3),
            "median": round(float(np.median(gd)), 3),
            "p95": round(float(np.percentile(gd, 95)), 3),
            "p99": round(float(np.percentile(gd, 99)), 3),
            "divergent_fraction": round(float((gd < -0.1).mean()), 4),
            "near_fraction": round(float((gd > 0.3).mean()), 4),
            "near_median": round(float(np.median(gd[gd > 0.3])), 3) if (gd > 0.3).any() else None,
            "divergent_median": round(float(np.median(gd[gd < -0.1])), 3) if (gd < -0.1).any() else None,
        }
        # what depth the 99th percentile means, for a sanity read: z = ipd / tan(disparity)
        p99 = float(np.percentile(gd, 99))
        summary["h_disparity_deg"]["p99_depth_m"] = round(ipd / math.tan(math.radians(p99)), 3) if p99 > 0.01 else None
    if vs.size:
        vdeg = np.degrees(np.arctan(vs / ppt))
        hdeg = np.degrees(np.arctan(hs / ppt))
        summary["v_disparity_deg"] = {
            "pixels": int(vs.size),
            "median": round(float(np.median(vdeg)), 3),
            "abs_p95": round(float(np.percentile(np.abs(vdeg), 95)), 3),
            "abs_max": round(float(np.abs(vdeg).max()), 3),
        }
        summary["flow_h_disparity_deg_median"] = round(float(np.median(hdeg)), 3)
    tiles = tile_match(grid, ppt)
    points = tiles.pop("points", [])
    summary["tile_disparity_deg"] = tiles
    summary["near_objects_vertical"] = near_objects_vertical(grid, ppt, points)
    if tiles.get("near") and tiles["near"]["h_median"] > 0:
        summary["tile_disparity_deg"]["near_depth_m"] = round(ipd / math.tan(math.radians(tiles["near"]["h_median"])), 3)

    # Pictures.
    pair = np.concatenate([imgs[0], np.full((imgs[0].shape[0], 8, 3), 255, np.uint8), imgs[1]], axis=1)
    put(pair, f"{tag}  left eye", 28)
    cv2.putText(pair, "right eye", (imgs[0].shape[1] + 18, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(pair, "right eye", (imgs[0].shape[1] + 18, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.imwrite(os.path.join(out, f"{tag}_eyes.png"), cv2.cvtColor(pair, cv2.COLOR_RGB2BGR))
    g0 = cv2.cvtColor(grid[0], cv2.COLOR_RGB2GRAY)
    g1 = cv2.cvtColor(grid[1], cv2.COLOR_RGB2GRAY)
    ana = np.stack([g0, g1, g1], axis=2)
    cv2.imwrite(os.path.join(out, f"{tag}_anaglyph.png"), cv2.cvtColor(ana, cv2.COLOR_RGB2BGR))
    dm = colorize(deg, good)
    hd = summary.get("h_disparity_deg", {})
    put(dm, f"{tag}: disparity deg (blue -1 .. red +6, magenta = divergent)", 26, 0.6)
    if hd:
        put(dm, f"median {hd['median']}  p99 {hd['p99']}  divergent {hd['divergent_fraction']}", 52, 0.6)
    vd = summary.get("v_disparity_deg")
    if vd:
        put(dm, f"vertical: median {vd['median']}  |p95| {vd['abs_p95']}", 78, 0.6)
    # The tile matches on top: a ring per confident tile, coloured on the same scale, its vertical
    # disparity as a tick (a vertical error of 0.1 deg is a 10 px tick).
    for (px, py, hdg, vdg) in points:
        t = min(max((hdg + 1.0) / 7.0, 0.0), 1.0)
        c = cv2.applyColorMap(np.uint8([[int(t * 255)]]), cv2.COLORMAP_TURBO)[0, 0]
        colour = (255, 0, 255) if hdg < -0.1 else (int(c[2]), int(c[1]), int(c[0]))
        cv2.circle(dm, (int(px), int(py)), 6, colour, 2, cv2.LINE_AA)
        cv2.line(dm, (int(px), int(py)), (int(px), int(py + vdg * 100)), (255, 255, 255), 1, cv2.LINE_AA)
    near = tiles.get("near")
    if near:
        put(dm, f"tiles near: {near['tiles']}  median {near['h_median']} deg  ({tiles.get('near_depth_m')} m)  "
                f"vertical |p95| {near['v_abs_p95']}", 104, 0.55)
    far = tiles.get("far")
    if far:
        put(dm, f"tiles far: {far['tiles']}  median {far['h_median']} deg  vertical |p95| {far['v_abs_p95']}  "
                f"divergent {far['divergent']}", 128, 0.55)
    cv2.imwrite(os.path.join(out, f"{tag}_disp.png"), cv2.cvtColor(dm, cv2.COLOR_RGB2BGR))
    if as_json:
        print(json.dumps(summary))
    else:
        print(json.dumps(summary, indent=1))
    return summary


def diff(folder, a, b, out, region="centre"):
    """Two composites of a static world taken with the eye NOT turning ("world" mode): with a
    truthful layer pose they are identical whatever the head did between them. `region` far: the
    upper third only, the distant world, away from the gun and hands, which idle between captures."""
    res = {}
    for i in (0, 1):
        ia = load_bmp(os.path.join(folder, f"SVR_seen_{a}_eye{i}.bmp"))
        ib = load_bmp(os.path.join(folder, f"SVR_seen_{b}_eye{i}.bmp"))
        both = ~(no_image_mask(ia) | no_image_mask(ib))
        ga = np.float32(cv2.cvtColor(ia, cv2.COLOR_RGB2GRAY))
        gb = np.float32(cv2.cvtColor(ib, cv2.COLOR_RGB2GRAY))
        # centre region only: the edges differ legitimately (what entered or left the field)
        h, w = ga.shape
        y0, y1, x0, x1 = h // 4, 3 * h // 4, w // 4, 3 * w // 4
        if region == "far":
            # The top third: sky and distant structures. Lower, the gun's top reaches ~38 % of the
            # eye's height, and it idles between two captures taken seconds apart.
            y0, y1, x0, x1 = h // 10, h // 3, w // 10, 9 * w // 10
        win = cv2.createHanningWindow((x1 - x0, y1 - y0), cv2.CV_32F)
        (sx, sy), resp = cv2.phaseCorrelate(ga[y0:y1, x0:x1], gb[y0:y1, x0:x1], win)
        sel = both.copy()
        if region == "far":
            sel[:y0, :] = False
            sel[y1:, :] = False
        mad = float(np.abs(ga - gb)[sel].mean()) if sel.any() else None
        res[f"eye{i}"] = {"shift_px": [round(sx, 2), round(sy, 2)], "response": round(resp, 3),
                          "mad": round(mad, 2) if mad is not None else None}
        if out:
            os.makedirs(out, exist_ok=True)
            d = np.clip(np.abs(ga - gb) * 3, 0, 255).astype(np.uint8)
            cv2.imwrite(os.path.join(out, f"{a}_vs_{b}_eye{i}.png"), d)
    print(json.dumps(res, indent=1))
    return res


def fidelity(folder, ref, test, out):
    """Angular fidelity: the same scene composed two ways must put every distant point at the same
    angle in the eye. `ref` is a composite whose geometry is trusted (the old desktop-shaped render,
    whose layer declares the projection the GPU received); `test` the one under test. Both are on the
    eye's own grid, linear in tan, so an exact image is the IDENTITY between them: features are matched
    (SIFT, ratio test), an affine fit by RANSAC, and its scale per axis, rotation and shift reported.
    A render squeezed into a band reads as a vertical scale far from 1; a wrong declared FOV as a scale
    on both axes; a wrong pose as a shift."""
    res = {}
    for i in (0, 1):
        a = load_bmp(os.path.join(folder, f"SVR_seen_{ref}_eye{i}.bmp"))
        b = load_bmp(os.path.join(folder, f"SVR_seen_{test}_eye{i}.bmp"))
        info = parse_info(os.path.join(folder, f"SVR_seen_{ref}.txt"))
        fov = info["eyes"][i]["fov"]
        h, w, _ = a.shape
        ppt_x = w / (tan_deg(fov[1]) - tan_deg(fov[0]))
        ppt_y = h / (tan_deg(fov[2]) - tan_deg(fov[3]))
        ga = cv2.cvtColor(a, cv2.COLOR_RGB2GRAY)
        gb = cv2.cvtColor(b, cv2.COLOR_RGB2GRAY)
        ma = (~no_image_mask(a)).astype(np.uint8) * 255
        mb = (~no_image_mask(b)).astype(np.uint8) * 255
        # The lower third is the gun and hands, which idle between two captures: the far world only.
        ma[int(h * 0.62):, :] = 0
        mb[int(h * 0.62):, :] = 0
        sift = cv2.SIFT_create(nfeatures=4000)
        ka, da = sift.detectAndCompute(ga, ma)
        kb, db = sift.detectAndCompute(gb, mb)
        if da is None or db is None or len(ka) < 20 or len(kb) < 20:
            res[f"eye{i}"] = {"matches": 0}
            continue
        pairs = cv2.BFMatcher().knnMatch(da, db, k=2)
        good = [m for m, n in (p for p in pairs if len(p) == 2) if m.distance < 0.7 * n.distance]
        pa = np.float32([ka[m.queryIdx].pt for m in good])
        pb = np.float32([kb[m.trainIdx].pt for m in good])
        if len(good) < 12:
            res[f"eye{i}"] = {"matches": len(good)}
            continue
        M, inl = cv2.estimateAffine2D(pa, pb, method=cv2.RANSAC, ransacReprojThreshold=2.0)
        inl = inl.ravel().astype(bool)
        sx = float(np.hypot(M[0, 0], M[1, 0]))
        sy = float(np.hypot(M[0, 1], M[1, 1]))
        rot = float(np.degrees(np.arctan2(M[1, 0], M[0, 0])))
        # the shift at the optical axis, in degrees
        cx = (0 - tan_deg(fov[0])) * ppt_x
        cy = (tan_deg(fov[2]) - 0) * ppt_y
        mx = M[0, 0] * cx + M[0, 1] * cy + M[0, 2] - cx
        my = M[1, 0] * cx + M[1, 1] * cy + M[1, 2] - cy
        resid = np.hypot(*(pb[inl] - (pa[inl] @ M[:, :2].T + M[:, 2])).T)
        res[f"eye{i}"] = {"matches": int(inl.sum()), "scale_x": round(sx, 4), "scale_y": round(sy, 4),
                          "rot_deg": round(rot, 3),
                          "shift_deg": [round(math.degrees(math.atan(mx / ppt_x)), 3),
                                        round(math.degrees(math.atan(my / ppt_y)), 3)],
                          "resid_px_median": round(float(np.median(resid)), 2)}
        if out:
            vis = cv2.drawMatches(cv2.cvtColor(a, cv2.COLOR_RGB2BGR), ka, cv2.cvtColor(b, cv2.COLOR_RGB2BGR), kb,
                                  [g for g, k in zip(good, inl) if k][:80], None, flags=2)
            cv2.imwrite(os.path.join(out, f"{ref}_vs_{test}_fidelity_eye{i}.png"), vis[::2, ::2])
    print(json.dumps(res))
    return res


def hand_depth(folder, tag_on, tag_off, out):
    """Is the drawn hand where the controller is, in DEPTH? Mono can never show it; stereo shows it at
    once. `tag_on` is a stereo composite with the arm module's overlay on (SVR_Arms.txt `overlay on`:
    the controller frames and the solved arm, drawn by world_lines at their true camera-relative
    positions), `tag_off` the same moment without it. The overlay's own disparity is the controller's
    true depth -- the instrument's ground truth; the tiles of the DRAWN image around the right
    controller's overlay (the glove on the grip) must give the same depth."""
    on = parse_info(os.path.join(folder, f"SVR_seen_{tag_on}.txt"))
    gon, ppt, _ = common_grid(on, [load_bmp(os.path.join(folder, f"SVR_seen_{tag_on}_eye{i}.bmp")) for i in (0, 1)])
    off = parse_info(os.path.join(folder, f"SVR_seen_{tag_off}.txt"))
    goff, _, _ = common_grid(off, [load_bmp(os.path.join(folder, f"SVR_seen_{tag_off}_eye{i}.bmp")) for i in (0, 1)])
    ipd = on.get("ipd", 0.064)
    masks = []
    for i in (0, 1):
        a = gon[i].astype(int)
        b = goff[i].astype(int)
        sat = (a.max(axis=2) - a.min(axis=2)) > 150
        masks.append(((np.abs(a - b).sum(axis=2) > 120) & sat).astype(np.float32))
    h, w = masks[0].shape
    left_half = masks[0][:, w // 2:]
    best = None
    for d in range(-20, 300):
        x0 = w // 2 - d
        if x0 < 0 or x0 + left_half.shape[1] > w:
            continue
        s = float((left_half * masks[1][:, x0:x0 + left_half.shape[1]]).sum())
        if best is None or s > best[1]:
            best = (d, s)
    res = {"overlay_px": int(masks[0].sum())}
    if best is None or masks[0].sum() < 200:
        print(json.dumps(res))
        return res
    deg = math.degrees(math.atan(best[0] / ppt))
    z_ctrl = ipd / math.tan(math.radians(deg)) if deg > 0.05 else None
    ys, xs = np.nonzero(left_half > 0)
    cx, cy = float(np.median(xs)) + w // 2, float(np.median(ys))
    tiles = tile_match(goff, ppt)
    pts = [p for p in tiles.get("points", []) if p[2] > 2.0 and abs(p[0] - cx) < 0.06 * w and abs(p[1] - cy) < 0.06 * h]
    zs = [ipd / math.tan(math.radians(p[2])) for p in pts]
    res.update({"overlay_deg": round(deg, 3), "controller_depth_m": round(z_ctrl, 3) if z_ctrl else None,
                "hand_tiles": len(zs), "hand_depth_m": round(float(np.median(zs)), 3) if zs else None,
                "hand_depth_p10_p90": [round(float(np.percentile(zs, 10)), 3), round(float(np.percentile(zs, 90)), 3)] if zs else None,
                "centre_px": [round(cx), round(cy)]})
    print(json.dumps(res))
    return res


def sequence(folder, tag, n, out):
    """Per frame of a composed sequence: which image each eye showed and whether it was new."""
    rows = []
    prev = {0: None, 1: None}
    for k in range(n):
        t = f"{tag}_f{k:02d}"
        p = os.path.join(folder, f"SVR_seen_{t}.txt")
        if not os.path.exists(p):
            break
        info = parse_info(p)
        row = {"f": k, "frame": info.get("frame")}
        for i in (0, 1):
            v = info["views"].get(i)
            if not v:
                row[f"e{i}"] = None
                continue
            key = (v["swap_ptr"], v["releases"])
            fresh = prev[i] is None or key != prev[i]
            prev[i] = key
            row[f"e{i}"] = {"swap": v["swap_ptr"][-4:], "rel": v["releases"], "fresh": fresh,
                           "p": [round(x, 4) for x in v["p"]]}
        rows.append(row)
    for r in rows:
        print(json.dumps(r))
    return rows


def main(argv):
    out = None
    ipd = None
    as_json = False
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--out":
            out = argv[i + 1]
            i += 2
        elif argv[i] == "--ipd":
            ipd = float(argv[i + 1])
            i += 2
        elif argv[i] == "--json":
            as_json = True
            i += 1
        else:
            args.append(argv[i])
            i += 1
    if not args:
        print(__doc__)
        return 2
    if args[0] == "diff":
        diff(args[1], args[2], args[3], out, region=args[4] if len(args) > 4 else "centre")
    elif args[0] == "seq":
        sequence(args[1], args[2], int(args[3]), out)
    elif args[0] == "fidelity":
        fidelity(args[1], args[2], args[3], out)
    elif args[0] == "handdepth":
        hand_depth(args[1], args[2], args[3], out)
    else:
        analyse(args[0], args[1], out or args[0], ipd, as_json)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
