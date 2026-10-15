"""Reads SVR_AdsTrace.csv (`trace <s>` in SVR_Ads.txt) and answers three questions about an ADS cycle.

    python ads_trace.py <SVR_AdsTrace.csv> [--rows]

1. Does the ADS animation move in step with the engine's FOV?  The compensation scales the learned
   hip->ADS move by the FOV-derived fraction, so the two must agree frame by frame. `alpha` is how far
   the drawn hand is along its own hip->ADS path (projection of its position), `f` the FOV fraction;
   the report gives their worst disagreement and whether the hand leads or lags.
2. Is the hand a good stand-in for the gun?  The weapon palette's root relative to the hand must not
   change through ADS; the report gives how much it does (mm, deg).
3. What does the player SEE?  `seen` is the drawn hand relative to the right controller. With the
   compensation on, it must not move through an ADS cycle; the report gives its worst excursion from
   the hip value (mm, deg), which is the visible slip of the gun in the hand.
"""
import csv
import math
import sys


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qangle(a, b):
    d = abs(sum(x * y for x, y in zip(a, b)))
    return 2.0 * math.degrees(math.acos(min(1.0, d)))


def qrot(q, v):
    p = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), qconj(q))
    return p[:3]


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def norm(a):
    return math.sqrt(sum(x * x for x in a))


def main():
    path = sys.argv[1]
    show = '--rows' in sys.argv
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            rows.append({k: float(v) for k, v in r.items()})
    if not rows:
        print('empty trace')
        return
    t0 = rows[0]['ms']
    for r in rows:
        r['t'] = r['ms'] - t0
        r['h'] = (r['hx'], r['hy'], r['hz'])
        r['hq'] = (r['hqx'], r['hqy'], r['hqz'], r['hqw'])
        r['g'] = (r['gx'], r['gy'], r['gz'])
        r['gq'] = (r['gqx'], r['gqy'], r['gqz'], r['gqw'])
        r['s'] = (r['sx'], r['sy'], r['sz'])
        r['sq'] = (r['sqx'], r['sqy'], r['sqz'], r['sqw'])
    zmax = max(r['zoom'] for r in rows)
    print(f'rows={len(rows)} span={rows[-1]["t"]:.0f} ms  zoom max={zmax:.4f}  fov min={min(r["fov"] for r in rows):.4f}')
    hip = [r for r in rows if r['zoom'] < 1.0005]
    ads = [r for r in rows if r['zoom'] > zmax - 0.0005]
    if not hip or not ads:
        print('no hip or no full-ADS rows')
        return
    h0 = hip[0]
    a0 = ads[len(ads) // 2]
    path_v = sub(a0['h'], h0['h'])
    plen = norm(path_v)
    print(f'hand hip->ads move: {plen * 1000:.1f} mm, turn {qangle(h0["hq"], a0["hq"]):.2f} deg (local frame)')
    worst = 0.0
    lead = []
    for r in rows:
        f_fov = (r['zoom'] - 1.0) / (zmax - 1.0) if zmax > 1.0001 else 0.0
        alpha = sum(x * y for x, y in zip(sub(r['h'], h0['h']), path_v)) / (plen * plen) if plen > 1e-6 else 0.0
        r['f'] = f_fov
        r['alpha'] = alpha
        if 0.02 < f_fov < 0.98 or 0.02 < alpha < 0.98:
            worst = max(worst, abs(alpha - f_fov))
            lead.append(alpha - f_fov)
    if lead:
        print(f'animation vs FOV fraction: worst |alpha - f| = {worst:.3f}; mean (alpha - f) = {sum(lead) / len(lead):+.3f} '
              f'over {len(lead)} transition rows ({"hand leads" if sum(lead) > 0 else "hand lags"})')
    gun_rows = [r for r in rows if r['gun'] > 0.5]
    if gun_rows:
        rel0 = None
        dmax = 0.0
        amax = 0.0
        for r in gun_rows:
            # gun relative to hand: hand^-1 * gun
            rq = qmul(qconj(r['hq']), r['gq'])
            rt = qrot(qconj(r['hq']), sub(r['g'], r['h']))
            if rel0 is None:
                rel0 = (rt, rq)
            dmax = max(dmax, norm(sub(rt, rel0[0])))
            amax = max(amax, qangle(rq, rel0[1]))
        print(f'gun root vs hand through the trace: {dmax * 1000:.1f} mm, {amax:.2f} deg ({len(gun_rows)} rows with a gun palette)')
    else:
        print('no weapon palette seen near the hand')
    s0 = hip[0]
    smax = 0.0
    sang = 0.0
    tmax = 0
    for r in rows:
        d = norm(sub(r['s'], s0['s']))
        if d > smax:
            smax = d
            tmax = r['t']
        sang = max(sang, qangle(r['sq'], s0['sq']))
    comp_rows = sum(1 for r in rows if r['comp'] > 0.5)
    print(f'visible slip (hand vs controller), worst through the trace: {smax * 1000:.1f} mm at t={tmax:.0f} ms, '
          f'{sang:.2f} deg; compensated rows {comp_rows}/{len(rows)}')
    if 'vx' in rows[0] and gun_rows:
        g0 = next(r for r in rows if r['gun'] > 0.5 and r['zoom'] < 1.0005)
        gmax = 0.0
        gang = 0.0
        gt = 0
        for r in gun_rows:
            v = (r['vx'], r['vy'], r['vz'])
            d = norm(sub(v, (g0['vx'], g0['vy'], g0['vz'])))
            if d > gmax:
                gmax = d
                gt = r['t']
            gang = max(gang, qangle((r['vqx'], r['vqy'], r['vqz'], r['vqw']), (g0['vqx'], g0['vqy'], g0['vqz'], g0['vqw'])))
        print(f'visible slip (GUN vs controller), worst through the trace: {gmax * 1000:.1f} mm at t={gt:.0f} ms, {gang:.2f} deg')
    if show:
        print('   t_ms     fov    zoom      f   alpha  comp  slip_mm slip_deg')
        prev = None
        for r in rows:
            if prev is not None and abs(r['zoom'] - prev['zoom']) < 1e-5 and abs(r['f'] - prev['f']) < 1e-5 and r['t'] - prev['t'] < 200:
                continue
            print(f'{r["t"]:7.0f} {r["fov"]:.4f} {r["zoom"]:.4f} {r["f"]:6.3f} {r["alpha"]:6.3f} {int(r["comp"]):4d} '
                  f'{norm(sub(r["s"], s0["s"])) * 1000:8.1f} {qangle(r["sq"], s0["sq"]):8.2f}')
            prev = r


if __name__ == '__main__':
    main()
