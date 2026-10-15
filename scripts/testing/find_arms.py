"""Lists the viewmodel's skinned meshes in the last GPU capture: vertex buffer, bone count, draws.

Viewmodel draws are told apart by their per-view constants (cb12): the camera sits at the origin
(row 7 is zero) because the viewmodel is drawn camera-relative. The arm skeleton is the largest
palette among them; meshes whose first bones repeat its first bones are arm meshes too.
"""
import sys
from collections import OrderedDict

import numpy as np

import gpu_capture as gc


def blob_of(rec, slot, blobs):
    v = rec.get(slot)
    if not v or ':b' not in v:
        return None
    return blobs.get(int(v.rsplit(':b', 1)[1]))


def rigid_bones(f):
    rest = f[32:]
    n = len(rest) // 12
    m = rest[:n * 12].reshape(n, 3, 4)
    R = m[:, :, :3]
    with np.errstate(all='ignore'):
        e = np.abs(np.einsum('nij,nkj->nik', R, R) - np.eye(3)).max(axis=(1, 2))
    k = 0
    while k < n and e[k] < 0.05:
        k += 1
    return m[:k]


def viewmodel_meshes(game=gc.GAME):
    draws, shaders, layouts, blobs = gc.load(game)
    meshes = OrderedDict()
    for r in draws:
        if r['kind'] != 'd':
            continue
        b12 = blob_of(r, 'cb12', blobs)
        b11 = blob_of(r, 'cb11', blobs)
        if b12 is None or b11 is None or len(b12) < 224:
            continue
        v = np.frombuffer(b12, '<f4').reshape(-1, 4)
        if np.abs(v[7, :3]).max() > 1e-3:
            continue  # world camera, not the viewmodel's
        f = np.frombuffer(b11, '<f4')
        m = rigid_bones(f)
        if len(m) == 0:
            continue
        vb = r.get('vb0', '-').split(':')[0]
        e = meshes.setdefault(vb, dict(bones=len(m), scale=float(f[16]), offset=f[20:23].copy(), draws=[],
                                       palette=m, vs=r.get('vsh'), stride=r.get('vb0', '').split(':')[1]))
        e['draws'].append(int(r['i']))
    return meshes


def main():
    meshes = viewmodel_meshes(sys.argv[1] if len(sys.argv) > 1 else gc.GAME)
    if not meshes:
        print('no viewmodel meshes')
        return
    skel = max(meshes.values(), key=lambda e: e['bones'])['palette']
    for vb, e in meshes.items():
        n = min(len(skel), e['bones'])
        same = np.abs(e['palette'][:n] - skel[:n]).max() < 1e-4
        role = 'arm' if same and e['bones'] >= 20 else 'other'
        print(f"{vb} bones={e['bones']} scale={e['scale']:.4f} stride={e['stride']} draws={e['draws'][:12]} role={role}")


if __name__ == '__main__':
    main()
