"""World positions of the body's key joints in both captures (joints solved across poses).

    python keypoints.py
"""
import numpy as np

import skel

PAIRS = {
    'hip L (1-3)': (1, 3), 'hip R (1-4)': (1, 4), 'pelvis-spine (1-5)': (1, 5), 'spine (5-8)': (5, 8),
    'neck-16 (13-16)': (13, 16), '16-18': (16, 18), 'neck-18 (13-18)': (13, 18),
    'shoulder L (12-15)': (12, 15), 'shoulder R (14-17)': (14, 17),
    'wrist L (19-21)': (19, 21), 'wrist R (20-22)': (20, 22), 'ankle L (6-9)': (6, 9), 'ankle R (7-10)': (7, 10),
}


def main():
    np.set_printoptions(precision=3, suppress=True)
    _, pal_sw, _, _ = skel.palettes('cap_sw')
    _, pal_1p, _, _ = skel.palettes('cap_1p')
    vm = skel.vm_palettes('cap_1p')
    s72 = max(pal_sw.values(), key=lambda e: e['bones'])['pal']
    p72 = max(pal_1p.values(), key=lambda e: e['bones'])['pal']
    v72 = max(vm.values(), key=lambda e: e['bones'])['pal']
    root = s72[0, :, 3]
    R0 = s72[0, :, :3]
    print('root', root)
    for lab, (p, c) in PAIRS.items():
        J, res, sv = skel.solve_joint([s72, p72], p, c)
        ws = s72[c, :, :3] @ J + s72[c, :, 3]
        w1 = p72[c, :, :3] @ J + p72[c, :, 3]
        print(f'{lab:20s} bindJ={J} res={res*1000:.2f}mm minsv={sv[-1]:.3f}  cap_sw={ws} (root frame {R0.T @ (ws - root)})  cap_1p={w1}')


if __name__ == '__main__':
    main()
