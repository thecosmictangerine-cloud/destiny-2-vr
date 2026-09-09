"""Fits the V that determines the weapon's pivot, and reports what the fit is worth.

The model, and every term in it earned its place:

    measured(p) = k * |D + p| + bias

`p` is the forward pivot the module was told to apply, `D` is the engine's own viewmodel offset
along the aim axis -- the number wanted -- `k` converts metres of weapon travel into pixels, and
`bias` is a measurement artefact, not a translation.

The bias is the whole reason this is not a one-line calculation. A hand YAW turns the weapon as well
as moving it, a yawed gun foreshortens, and normalised cross-correlation answers with the offset
that best fits a CHANGED shape. On this scene that is worth around 100 px with no translation behind
it. Between p = 0 and p = -D the model is linear in p, so a fit over that range cannot separate `k*D`
from `bias`: the two are the same number to a straight line. Crossing the vertex is what breaks the
degeneracy, because past it the slope changes sign.

So: fit both branches, and report the vertex. If the scan never crossed, say so rather than
returning a number that is really just an intercept.

Usage: python pivot_fit.py <scan.csv>
"""

import csv
import sys

import numpy as np


def read(path):
    p, m = [], []
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            p.append(float(row["PivotFwd"]))
            m.append(float(row["YawMag"]))
    return np.array(p), np.array(m)


def fit_v(p, m):
    """@return (D, k, bias, rms) of the best fit over a grid of candidate vertices."""
    best = None
    for candidate in np.arange(0.02, 1.20, 0.005):
        basis = np.stack([np.abs(candidate + p), np.ones_like(p)], axis=1)
        solution, *_ = np.linalg.lstsq(basis, m, rcond=None)
        residual = m - basis @ solution
        rms = float(np.sqrt((residual ** 2).mean()))
        if solution[0] <= 0:
            continue  # a negative pixels-per-metre is not a fit, it is the solver giving up
        if best is None or rms < best[3]:
            best = (float(candidate), float(solution[0]), float(solution[1]), rms)
    return best


def main():
    p, m = read(sys.argv[1])
    if len(p) < 4:
        print("pivot_fit: need at least four points")
        return 1
    print(f"  points      {len(p)}")
    for a, b in zip(p, m):
        print(f"    p {a:+.3f}   measured {b:6.0f} px")

    best = fit_v(p, m)
    if best is None:
        print("  no usable fit")
        return 1
    D, k, bias, rms = best
    print(f"  |d|         {D:.3f} m")
    print(f"  px per m    {k:.0f}")
    print(f"  bias        {bias:+.0f} px   <- measurement artefact, not weapon travel")
    print(f"  fit rms     {rms:.1f} px")

    # Whether the scan actually bracketed the vertex decides if this is a measurement or an
    # extrapolation. Without points on both sides the "vertex" is wherever the straight line
    # through the data happens to cross, which is not the same thing at all.
    crossed = (p.min() < -D) and (p.max() > -D)
    print(f"  bracketed   {'YES' if crossed else 'NO'}"
          f"   (scan spans {p.min():+.2f} to {p.max():+.2f}, vertex at {-D:+.2f})")
    if not crossed:
        print("  WARNING: the scan did not cross the vertex, so |d| here is an extrapolation and")
        print("           cannot be separated from the constant bias. Widen the scan.")
    print(f"  expected    0.33 m from RESEARCH.md, by an unrelated route")
    print(f"PIVOT {-D:.4f} 0 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
