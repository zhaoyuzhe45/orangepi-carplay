#!/usr/bin/env python3
"""Measure where the desktop actually sits inside a screenrecord capture.

Answers the question "is the stream we push the whole desktop, or a crop /
stretch of it" without trusting any log line: it registers two images of the
same static screen by their row-brightness profiles and reports the vertical
scale factor and offset.

Typical use on this project (panel is fixed 1920x1080, wm override is the
canvas):

    # 1. on the box, capture with the service's own parameters
    screenrecord --size 1920x1080 --time-limit 2 --output-format=h264 /data/local/tmp/a.h264
    screencap -p /data/local/tmp/panel.png
    # 2. decode one frame, dump both to 8-bit gray
    ffmpeg -i a.h264 -frames:v 1 -f image2 cap.png
    ffmpeg -i cap.png -f rawvideo -pix_fmt gray cap.gray
    ffmpeg -i panel.png -f rawvideo -pix_fmt gray panel.gray
    # 3. register
    frame_geometry.py cap.gray:1080 panel.gray:1080

Readings:
  scale=1.000  -> the two images are the same 1:1 picture, only shifted
  scale=1.200  -> the destination is vertically squashed/stretched by 1.2

Measured 2026-10-27 (wm 1920x900, panel 1920x1080):
  capture@1920x1080 vs panel   -> scale 1.000, offset +90
      the capture holds the 900-row display frame 1:1 at rows 0..899, the
      panel's top letterbox removed; the SPS crop (0,0,1920,900) is exact.
  capture@1920x900  vs panel   -> scale 1.200, offset +90
      requesting a height other than the panel squeezes the panel into it,
      which is why the capture size must always be the panel size.
"""

import sys


def load(spec):
    path, _, h = spec.partition(":")
    data = open(path, "rb").read()
    height = int(h)
    width = len(data) // height
    return path, width, height, data


def row_signal(width, height, data, x0f=0.05, x1f=0.47):
    x0 = int(width * x0f)
    x1 = int(width * x1f)
    return [sum(data[y * width + x0:y * width + x1]) / (x1 - x0)
            for y in range(height)]


def best_match(src, dst):
    best = None
    scale = 0.80
    while scale <= 1.30:
        offset = -200.0
        while offset <= 400.0:
            err = n = 0.0
            for y in range(0, len(dst), 3):
                i = int(round(scale * y + offset))
                if 0 <= i < len(src):
                    err += abs(dst[y] - src[i])
                    n += 1
            if n > 200 and (best is None or err / n < best[0]):
                best = (err / n, scale, offset)
            offset += 2.0
        scale += 0.02
    e, s, b = best
    for ds in (-0.01, -0.005, -0.002, 0, 0.002, 0.005, 0.01):
        for db in (-1.5, -0.75, 0, 0.75, 1.5):
            err = n = 0.0
            for y in range(0, len(dst), 3):
                i = int(round((s + ds) * y + (b + db)))
                if 0 <= i < len(src):
                    err += abs(dst[y] - src[i])
                    n += 1
            if n > 200 and err / n < e:
                e, s, b = err / n, s + ds, b + db
    return s, b, e


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    sp, sw, sh, sdata = load(argv[1])
    dp, dw, dh, ddata = load(argv[2])
    if sw != dw:
        print("width mismatch: %s=%d %s=%d" % (sp, sw, dp, dw))
        return 1
    s, b, e = best_match(row_signal(sw, sh, sdata), row_signal(dw, dh, ddata))
    print("source %s  %dx%d" % (sp, sw, sh))
    print("dest   %s  %dx%d" % (dp, dw, dh))
    print("registration: dest(y) = source(%.3f*y + %.2f)  meanAbsErr=%.2f"
          % (s, b, e))
    if abs(s - 1.0) <= 0.01:
        print("verdict: same 1:1 picture, offset %+.0f rows" % b)
    else:
        print("verdict: geometry changed by %.3f" % s)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
