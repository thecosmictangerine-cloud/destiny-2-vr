"""Reads SVR_FrameTrace.csv (vr_frames.cpp): which head pose each consumer used, per frame.

For every present it answers:
  drawn     the publication serial the world was REALLY rendered with: the world view the GPU
            received last before this present, matched by position and forward against what the
            camera hook wrote (each camera note carries the serial it used).
  declared  the serial the layer declared for this image.
  lag       declared - drawn. Must be 0: anything else is a layer claiming a pose the image was
            not rendered from (swim in mono, a wrong eye with alternating eyes).
And per camera write: the serial the weapon (getter, transform) and the arms used after it, which
must be the camera's, or the gun and arms are drawn relative to a camera that is not the frame's.

usage: frame_trace.py <SVR_FrameTrace.csv> [--verbose]
"""
import csv
import sys
from collections import Counter

KIND = {0: "publish", 1: "camera", 2: "weapon", 3: "xform", 4: "arms", 5: "world", 6: "present", 7: "vm", 8: "mark"}


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({
                "t": float(r["us"]), "tid": int(r["tid"]), "kind": KIND.get(int(r["kind"]), r["kind"]),
                "serial": int(r["serial"]), "eye": int(r["eye"]),
                "v": [float(r[f"v{i}"]) for i in range(6)],
            })
    rows.sort(key=lambda r: r["t"])
    return rows


def close(a, b, tol_p=2e-3, tol_f=2e-4):
    return all(abs(a[i] - b[i]) < tol_p for i in range(3)) and all(abs(a[3 + i] - b[3 + i]) < tol_f for i in range(3))


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    verbose = "--verbose" in argv
    rows = load(argv[0])
    threads = Counter((r["kind"], r["tid"]) for r in rows)
    print("events by kind/thread:", dict(sorted(((f"{k}@{t}", n) for (k, t), n in threads.items()))))
    cameras = [r for r in rows if r["kind"] == "camera"]
    worlds = [r for r in rows if r["kind"] == "world"]
    presents = [r for r in rows if r["kind"] == "present"]
    publishes = [r for r in rows if r["kind"] == "publish"]
    print(f"publishes {len(publishes)}  cameras {len(cameras)}  world views {len(worlds)}  presents {len(presents)}")

    # Which serial does each world view correspond to: the camera write it equals (latest first).
    def match_world(w):
        best = None
        for c in reversed(cameras):
            if c["t"] > w["t"]:
                continue
            if close(c["v"], w["v"]):
                best = c
                break
        return best

    marks = [r for r in rows if r["kind"] == "mark"]
    if marks:
        behind = Counter(int(m["v"][0]) - m["serial"] for m in marks)
        print("camera sample: newest publication - serial taken:", dict(sorted(behind.items())))
    lags = Counter()
    unmatched = 0
    eye_mismatch = 0
    eye_seq = []
    prev_t = -1.0
    report = []
    for p in presents:
        if p["serial"] == 0:
            prev_t = p["t"]
            continue
        ws = [w for w in worlds if prev_t < w["t"] <= p["t"]]
        prev_t = p["t"]
        if not ws:
            report.append((p["t"], p["serial"], None, None, len(ws)))
            unmatched += 1
            continue
        c = match_world(ws[-1])
        if c is None:
            unmatched += 1
            report.append((p["t"], p["serial"], None, None, len(ws)))
            continue
        lag = p["serial"] - c["serial"]
        if lag != 0:
            # With the head held still several frames carry the very same camera, and which of them
            # is "the" match is a guess: the declared record is right if ITS camera is the drawn one.
            declared = [k for k in cameras if k["serial"] == p["serial"]]
            if declared and close(declared[-1]["v"], ws[-1]["v"]):
                lag = 0
        lags[lag] += 1
        if p["eye"] != c["eye"]:
            eye_mismatch += 1
        eye_seq.append(p["eye"])
        report.append((p["t"], p["serial"], c["serial"], lag, len(ws)))
    print("present lag (declared - drawn):", dict(sorted(lags.items())), " unmatched", unmatched,
          " eye declared != eye drawn:", eye_mismatch)
    if eye_seq:
        repeats = sum(1 for a, b in zip(eye_seq, eye_seq[1:]) if a == b)
        print("present eyes:", "".join(str(e) for e in eye_seq[:80]), " consecutive repeats", repeats)
    if verbose:
        for t, d, dr, lag, n in report[:60]:
            print(f"  t={t/1000:9.2f}ms declared={d} drawn={dr} lag={lag} worldviews={n}")

    # Consumers after each camera write on the same thread, before the next camera write.
    for kind in ("weapon", "xform", "arms"):
        evs = [r for r in rows if r["kind"] == kind]
        if not evs:
            print(f"{kind}: no events")
            continue
        diffs = Counter()
        for e in evs:
            prior = [c for c in cameras if c["t"] <= e["t"]]
            if not prior:
                continue
            diffs[e["serial"] - prior[-1]["serial"]] += 1
        print(f"{kind} serial - latest camera serial:", dict(sorted(diffs.items())),
              " threads", sorted(set(e["tid"] for e in evs)))

    # The arms are posed on the render threads, while the camera hook is already building the NEXT
    # frame, so "the latest camera" is the wrong reference for them: what matters is the frame the GPU
    # is drawing, named by the latest world view before the arms' rewrite (any thread). In alternating
    # stereo a mismatch here is an arm posed against the other eye's camera.
    arms = [r for r in rows if r["kind"] == "arms"]
    if arms and worlds:
        drawn_diff = Counter()
        wi = 0
        ordered_worlds = sorted(worlds, key=lambda r: r["t"])
        matched = {}
        for e in arms:
            while wi + 1 < len(ordered_worlds) and ordered_worlds[wi + 1]["t"] <= e["t"]:
                wi += 1
            w = ordered_worlds[wi]
            if w["t"] > e["t"]:
                continue
            key = id(w)
            if key not in matched:
                matched[key] = match_world(w)
            c = matched[key]
            if c is None:
                drawn_diff["?"] += 1
                continue
            drawn_diff[e["serial"] - c["serial"]] += 1
        print("arms serial - drawn frame serial:", dict(sorted(drawn_diff.items(), key=lambda kv: str(kv[0]))))

    # Timing: per present, where in the frame each kind falls (ms after the previous present).
    if presents:
        span = (presents[-1]["t"] - presents[0]["t"]) / 1000.0
        print(f"presents/s {1000.0 * (len(presents) - 1) / span:.1f}" if span > 0 else "")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
