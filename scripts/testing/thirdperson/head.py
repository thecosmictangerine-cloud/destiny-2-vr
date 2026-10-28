"""Q2: which bone is the head, and where it is.

Candidates are the children of the neck (13): 16 and 18 (joints at z 1.61/1.65 in bind space).
For each candidate, prints the world position of: its joint with the neck, the helmet mesh's
bind-space centre (palette header origin of the 19-bone helmet palette), and a nominal eye
point (0.10, 0, 1.70) -- in cap_sw and in cap_1p's shadow body, and in the 1P viewmodel
palette (camera-relative: the eye should come out near the origin).

    python head.py
"""
import numpy as np

import skel


def main():
    np.set_printoptions(precision=4, suppress=True)
    cam_sw, pal_sw, _, _ = skel.palettes('cap_sw')
    cam_1p, pal_1p, _, _ = skel.palettes('cap_1p')
    vm = skel.vm_palettes('cap_1p')
    s72 = max(pal_sw.values(), key=lambda e: e['bones'])['pal']
    p72 = max(pal_1p.values(), key=lambda e: e['bones'])['pal']
    v72 = max(vm.values(), key=lambda e: e['bones'])['pal']
    helmet = [e for e in pal_sw.values() if e['bones'] == 19][0]
    hc = helmet['origin']
    poses = [s72, p72, v72]
    J13_16, r1, _ = skel.solve_joint(poses, 13, 16)
    J13_18, r2, _ = skel.solve_joint(poses, 13, 18)
    J11_13, r3, sv3 = skel.solve_joint(poses, 11, 13)
    J8_11, r4, sv4 = skel.solve_joint(poses, 8, 11)
    print('neck-16 joint', J13_16, r1, ' neck-18 joint', J13_18, r2)
    print('chest11-neck13 joint', J11_13, r3, sv3, ' 8-11 joint', J8_11, r4, sv4)
    eye = np.array([0.10, 0.0, 1.70])
    for tag, P, cam in (('cap_sw 3P', s72, cam_sw), ('cap_1p 3P (shadow)', p72, cam_1p), ('cap_1p VM (cam-relative)', v72, np.zeros(3))):
        print(f'== {tag}: camera {cam}')
        for k in (11, 13, 16, 18):
            M = P[k]
            print(f'  bone {k:2d}: M*helmetCentre={M[:, :3] @ hc + M[:, 3]}  M*eye={M[:, :3] @ eye + M[:, 3]}  '
                  f'M*J(13-16)={M[:, :3] @ J13_16 + M[:, 3]}  rel. rot to bone13 angle='
                  f'{np.degrees(np.arccos(np.clip((np.trace(P[13][:, :3].T @ M[:, :3]) - 1) / 2, -1, 1))):.1f} deg')
    # highest bones: world z of the helmet centre mapped by every bone (top of body)
    print('\nbones 0-71 in cap_sw: M_k * helmet centre z (top 8):')
    z = [(float((s72[k][:, :3] @ hc + s72[k][:, 3])[2]), k) for k in range(72)]
    print(sorted(z, reverse=True)[:8])


if __name__ == '__main__':
    main()
