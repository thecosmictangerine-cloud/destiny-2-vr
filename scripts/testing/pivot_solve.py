"""Solves for the weapon's pivot correction from screen-space measurements.

The point of this file is that the pivot is MEASURED, not guessed. What makes that possible is
that the quantity being driven to zero is an affine function of the thing being tuned.

The engine draws the weapon at `camera + R(q) * d + lanes`, and the module writes
`lanes = (palm - head) + R(q) * pivot`. Rotate the controller with its position held, from R1 to
R2, and the weapon's world displacement is

    D  =  (R2 - R1) * (pivot + d)

which is affine in `pivot` and zero exactly when `pivot = -d`. The screen displacement is a
projection of that, so it is affine too, locally, and it vanishes at the same point. So probing
each axis of `pivot` in turn gives the Jacobian by finite differences, and one linear solve gives
the correction.

Two rotations are used, not one. A single rotation is blind to the component of `pivot` along its
own axis -- that part of the lever arm does not move at all -- so one rotation leaves the problem
underdetermined. Two independent axes give four equations for three unknowns, and the least-squares
residual is then a bonus: it says whether the affine model actually holds.

Iteration is expected rather than a fallback. The pixels-per-metre factor depends on how far the
gun is from the eye, which changes as `pivot` changes, so the linearisation is only good near the
current point -- but the error shrinks with the displacement, so a second pass lands on it.

Usage: python pivot_solve.py <measurements.csv>

The CSV has one row per probe, columns: probe,pf,pr,pu,dx_a,dy_a,dx_b,dy_b
where `probe` is `base` or an axis name, `p*` is the pivot that row was measured with, and the four
`d*` are the weapon's screen displacement under rotation A and rotation B.
"""

import csv
import sys

import numpy as np

AXES = ("fwd", "right", "up")


def read(path):
    rows = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            rows[row["probe"]] = (
                np.array([float(row["pf"]), float(row["pr"]), float(row["pu"])]),
                np.array([float(row["dx_a"]), float(row["dy_a"]), float(row["dx_b"]), float(row["dy_b"])]),
            )
    return rows


def main():
    rows = read(sys.argv[1])
    if "base" not in rows:
        print("pivot_solve: no base row")
        return 1
    pivot0, residual0 = rows["base"]
    columns = []
    used = []
    for axis in AXES:
        if axis not in rows:
            continue
        pivot, residual = rows[axis]
        step = float(np.dot(pivot - pivot0, np.eye(3)[AXES.index(axis)]))
        if abs(step) < 1e-6:
            continue
        columns.append((residual - residual0) / step)
        used.append(AXES.index(axis))
    if len(columns) < 2:
        print("pivot_solve: need at least two axis probes")
        return 1

    jacobian = np.stack(columns, axis=1)
    # Least squares on `jacobian @ delta = -residual0`. rcond is pinned rather than left to the
    # numpy default so an axis the rotations happen to be insensitive to is damped instead of
    # blowing up into a metre-scale "correction".
    delta, _, rank, singular = np.linalg.lstsq(jacobian, -residual0, rcond=1e-3)
    solution = pivot0.copy()
    for slot, axis in enumerate(used):
        solution[axis] += delta[slot]
    predicted = residual0 + jacobian @ delta

    print("pivot_solve")
    print(f"  base pivot      {pivot0[0]:+.4f} {pivot0[1]:+.4f} {pivot0[2]:+.4f}")
    print(f"  base residual   {residual0[0]:+7.1f} {residual0[1]:+7.1f} {residual0[2]:+7.1f} {residual0[3]:+7.1f} px"
          f"   |r| {np.linalg.norm(residual0):.1f}")
    print(f"  jacobian rank   {rank}  singular {np.array2string(singular, precision=1)}")
    for slot, axis in enumerate(used):
        column = jacobian[:, slot]
        print(f"  d/d{AXES[axis]:<6}      {column[0]:+7.1f} {column[1]:+7.1f} {column[2]:+7.1f} {column[3]:+7.1f} px/m")
    print(f"  predicted resid {predicted[0]:+7.1f} {predicted[1]:+7.1f} {predicted[2]:+7.1f} {predicted[3]:+7.1f} px"
          f"   |r| {np.linalg.norm(predicted):.1f}")
    print(f"  SOLUTION        {solution[0]:+.4f} {solution[1]:+.4f} {solution[2]:+.4f}")
    print(f"  |pivot|         {np.linalg.norm(solution):.4f} m")
    # The independent check on the whole exercise: the engine's own viewmodel offset was measured
    # at about 0.33 m by a completely different route (0.08 m of lane travel subtending 13.5
    # degrees). If |pivot| lands near that, the number is real rather than a fudge that happens to
    # cancel one test.
    print(f"  expected |d|    0.33 m from RESEARCH.md -- agreement is the check that this is real")
    print(f"PIVOT {solution[0]:.4f} {solution[1]:.4f} {solution[2]:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
