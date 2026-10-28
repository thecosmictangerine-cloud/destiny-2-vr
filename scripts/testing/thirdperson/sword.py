"""Q3: the sword and the stowed weapon in cap_sw (and the 3P hand cannon in cap_1p's shadows).

- prints each weapon palette (1024-byte per-object cb11), its bone matrices, and the transform
  relative to the right/left hand bones (22/21) of the body palette: a rigid attachment shows up
  as a clean offset;
- dumps the VS cb0 window of the sword draws (per-object block);
- scans every draw's VS constants (all slots but cb11/cb12) and PS constants for float triples that
  sit within 0.6 m of the sword grip, to catch trails/effects that carry their own transform.

    python sword.py
"""
import numpy as np

import body
import cap


def h(m):
    M = np.eye(4)
    M[:3, :] = m
    return M


def main():
    np.set_printoptions(precision=4, suppress=True)
    for name, wanted in (('cap_sw', None), ('cap_1p', None)):
        cam, g, draws, blobs = body.groups(name, 4.0)
        skel = max((e for e in g.values()), key=lambda e: e['bones'])['pal']
        R0 = skel[0, :, :3]
        t0 = skel[0, :, 3]
        print(f'===== {name}  root {t0}')
        seen = set()
        for (vb, bid), e in g.items():
            if e['bones'] > 8 or bid in seen:
                continue
            seen.add(bid)
            m0 = e['pal'][0]
            local = R0.T @ (m0[:, 3] - t0)
            rel22 = np.linalg.inv(h(skel[22])) @ h(m0)
            rel21 = np.linalg.inv(h(skel[21])) @ h(m0)
            # bind-space extent of the mesh: origin +- scale
            o, s = e['origin'], e['scale']
            tip_w = m0[:, :3] @ (o + np.array([0, 0, s])) + m0[:, 3]
            print(f'-- b{bid} bones={e["bones"]} s={s:.4f} origin={o} draws={e["draws"]} passes={dict(e["passes"])}')
            print('   bone0 3x4:\n', m0)
            print('   bone0 translation in character root frame (X fwd, Y left, Z up):', local)
            print('   M_hand22^-1 * M_obj  translation:', rel22[:3, 3], ' |t|=', np.linalg.norm(rel22[:3, 3]))
            print('   M_hand21^-1 * M_obj  translation:', rel21[:3, 3], ' |t|=', np.linalg.norm(rel21[:3, 3]))
            if e['bones'] > 1:
                for k in range(e['bones']):
                    print(f'     bone{k} t={e["pal"][k][:, 3]}  R row0={e["pal"][k][0, :3]}')
        # hand joints in world
        for lab, (p, c, J) in {'wrist R': (20, 22, (0.2918, -0.3922, 1.1552)), 'wrist L': (19, 21, (0.2926, 0.3921, 1.155))}.items():
            J = np.array(J)
            print(f'   {lab} world = {skel[c, :, :3] @ J + skel[c, :, 3]}')
        # palm centre: roughly 8 cm past the wrist along the hand
        if name == 'cap_sw':
            grip = None
            for (vb, bid), e in g.items():
                if bid == 178:
                    grip = e['pal'][0][:, 3]
            print('\n   cb0 window of the sword draws:')
            for r in draws:
                if r['i'] in (205, 206):
                    f = cap.cbf(r, 0, blobs)
                    print(f'   draw {r["i"]} cb0 {r["vcb"][0][:4]} floats:\n', f.reshape(-1, 4))
            print('\n   draws whose VS/PS constants (not cb11/cb12) hold a point within 0.6 m of the sword grip', grip)
            hits = []
            for r in draws:
                for stage in ('vcb', 'pcb'):
                    for slot, e in r[stage].items():
                        if stage == 'vcb' and slot in (11, 12):
                            continue
                        f = cap.cbf(r, slot, blobs, stage)
                        if f is None or len(f) < 4:
                            continue
                        rows = f[:len(f) // 4 * 4].reshape(-1, 4)
                        d = np.linalg.norm(rows[:, :3] - grip, axis=1)
                        # also 3x4 matrices: translations in column 3 of rows k..k+2
                        k = np.where(d < 0.6)[0]
                        if len(k):
                            hits.append((r['i'], stage, slot, int(k[0]), rows[k[0], :3], r.get('vsh'), body.pass_label(r, blobs)))
                        if len(rows) >= 3:
                            tcol = np.stack([rows[:-2, 3], rows[1:-1, 3], rows[2:, 3]], 1)
                            d2 = np.linalg.norm(tcol - grip, axis=1)
                            k2 = np.where(d2 < 0.6)[0]
                            if len(k2):
                                hits.append((r['i'], stage, slot, 'mat@row%d' % k2[0], tcol[k2[0]], r.get('vsh'), body.pass_label(r, blobs)))
            for hit in hits[:80]:
                print('   ', hit)
            print('   total hits', len(hits))


if __name__ == '__main__':
    main()
