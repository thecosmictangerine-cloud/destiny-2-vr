"""Does the drawn gun stay in the hand? Reads an ADS trace (SVR_AdsTrace.csv, `trace <s>` in SVR_Ads.txt).

    python in_hand.py <trace.csv>

The trace's `seen gun` columns (vx..vqw) are the weapon palette's root relative to the right
controller's grip frame, every frame, as the GPU drew it. If the gun is rigidly in the hand that pose
is the same in every frame, whatever the controller's rotation or the room anchor's turn -- a test of
the weapon's placement with no camera, template or projection in it.

Only SETTLED frames are judged: the drawn pose reaches the GPU a frame after the controller is read,
so a controller that jumps (the mock moves instantly) shows its own motion for a frame or two. A
frame is settled when the pose has not changed by more than 0.5 mm / 0.1 deg over the 8 before it.

Prints `INHAND <worst mm> <worst deg> <settled frames>`: the worst settled frame's distance from the
median settled pose.
"""
import csv
import math
import sys


def quat_angle(a, b):
    d = abs(sum(x * y for x, y in zip(a, b)))
    return 2.0 * math.degrees(math.acos(min(1.0, d)))


def main():
    rows = [r for r in csv.DictReader(open(sys.argv[1])) if r.get('gun') == '1']
    poses = [([float(r[k]) for k in ('vx', 'vy', 'vz')], [float(r[k]) for k in ('vqx', 'vqy', 'vqz', 'vqw')])
             for r in rows]
    window = 8
    settled = []
    for i in range(window, len(poses)):
        t, q = poses[i]
        if all(math.dist(t, poses[j][0]) < 0.0005 and quat_angle(q, poses[j][1]) < 0.1 for j in range(i - window, i)):
            settled.append(poses[i])
    if len(settled) < 20:
        print(f'only {len(settled)} settled frames of {len(poses)}')
        return
    med = [sorted(p[0][k] for p in settled)[len(settled) // 2] for k in range(3)]
    ref = settled[len(settled) // 2][1]
    acc = [0.0] * 4
    for _, q in settled:
        s = 1.0 if sum(x * y for x, y in zip(q, ref)) >= 0 else -1.0
        acc = [a + s * x for a, x in zip(acc, q)]
    n = math.sqrt(sum(a * a for a in acc))
    mq = [a / n for a in acc]
    dt = sorted(math.dist(t, med) * 1000.0 for t, _ in settled)
    dq = sorted(quat_angle(q, mq) for _, q in settled)
    p95 = int(len(dt) * 0.95)
    print(f'{len(poses)} frames with the gun, {len(settled)} settled')
    print(f'distance from the median: p50 {dt[len(dt) // 2]:.2f} mm, p95 {dt[p95]:.2f} mm, worst {dt[-1]:.2f} mm')
    print(f'angle from the mean:      p50 {dq[len(dq) // 2]:.3f} deg, p95 {dq[p95]:.3f} deg, worst {dq[-1]:.3f} deg')
    print(f'INHAND {dt[-1]:.2f} {dq[-1]:.3f} {len(settled)}')


if __name__ == '__main__':
    main()
