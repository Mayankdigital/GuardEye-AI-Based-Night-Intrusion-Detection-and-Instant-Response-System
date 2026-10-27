#!/usr/bin/env python3
"""Make a synthetic test video: empty room, then a 'person' (rectangle) walks into
the restricted zone and stays, then leaves. Lets you test the whole pipeline
without a camera:   python3 scripts/make_test_video.py test.mp4
Needs: pip install opencv-python-headless numpy"""
import sys
import cv2
import numpy as np

out = sys.argv[1] if len(sys.argv) > 1 else "test.mp4"
W, H, FPS = 640, 360, 20
vw = cv2.VideoWriter(out, cv2.VideoWriter_fourcc(*"mp4v"), FPS, (W, H))
rng = np.random.default_rng(1)
base = np.full((H, W, 3), 45, np.uint8)
cv2.rectangle(base, (30, 40), (150, 140), (70, 70, 70), -1)      # a shelf, static background

for i in range(FPS * 40):
    t = i / FPS
    f = base.copy()
    f = cv2.add(f, rng.integers(0, 4, f.shape, dtype=np.uint8))  # camera noise
    if 8 <= t < 28:                                                # person enters at 8 s, leaves at 28 s
        x = int(np.interp(t, [8, 12, 24, 28], [-80, 300, 320, 700]))
        cv2.rectangle(f, (x, 130), (x + 70, 330), (190, 190, 190), -1)   # feet at y=330 -> 0.92 (inside zone)
        cv2.circle(f, (x + 35, 115), 18, (190, 190, 190), -1)
    vw.write(f)
vw.release()
print("wrote", out)
