#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic, redistributable NV12 benchmark clip; not camera evidence.

Requires numpy only for fast generation. Moving edges, noise/gain changes,
clipped pixels and scene cuts exercise temporal state on an offline sequence.
"""
import argparse
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output')
p.add_argument('--width', type=int, default=1920)
p.add_argument('--height', type=int, default=1080)
p.add_argument('--frames', type=int, default=90)
a = p.parse_args()
if min(a.width, a.height) < 16 or (a.width | a.height) & 1 or a.frames < 2:
    p.error('use even dimensions >= 16 and at least two frames')
rng = np.random.default_rng(4300)
y, x = np.mgrid[:a.height, :a.width]
yc, xc = np.mgrid[:a.height//2, :a.width]
with open(a.output, 'wb') as out:
    for t in range(a.frames):
        clean = 40 + 140*x/a.width + 20*((y//120) % 2)
        bx = (t*13) % a.width
        clean[(x >= bx) & (x < bx+a.width//6) & (y > a.height//3) & (y < 2*a.height//3)] = 25
        sigma = 12 if (t//30) % 2 == 0 else 4
        if t % 45 == 44:
            clean = 255-clean
        luma = np.clip(np.rint(clean+rng.normal(0, sigma, clean.shape)), 0, 255).astype('uint8')
        chroma = np.clip(np.rint(128+10*(xc % 2)+rng.normal(0, sigma/2, xc.shape)), 0, 255).astype('uint8')
        out.write(luma.tobytes())
        out.write(chroma.tobytes())
