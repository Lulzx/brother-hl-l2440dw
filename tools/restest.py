#!/usr/bin/env python3
"""restest.py -- what does RAS1200MODE actually do?  Raw PBM on stdout.

The engine is not square.  SNMP prtMarkerAddressability reports 600 dpi in the
feed direction (down the page, one laser scanline per step of the drum) and
2400 dpi in the cross-feed direction (across the page, laser modulation along a
single scan).  So a literal 1200x1200 grid is not something this hardware can
address, yet PJL offers RESOLUTION=1200/HQ1200/TR1200 and a separate
RAS1200MODE flag, and INQUIRE confirms the firmware also knows RESOLUTIONX and
RESOLUTIONY as independent values.

brlaser -- and therefore brpdf -- treats 1200 dpi as "set RAS1200MODE = TRUE,
send RESOLUTION = 600, then encode the bitmap exactly as at 600 dpi".  That is
an inherited guess.  Nobody has checked what the engine does with it.

This page is built to answer three questions with a loupe and a ruler:

  1. GEOMETRY.  Does a 1200 dpi bitmap come out the same physical size as a 600
     dpi one?  The mm rulers and the 1-inch step blocks read scale directly.  A
     page at half or double height means the engine disagrees with brlaser
     about how many rows it is being sent.

  2. IS THE EXTRA RESOLUTION REAL?  Gratings are specified in line pairs per
     inch, not in pixels, so the same physical target is drawn at both
     resolutions.  300 lp/in cannot exist in a 600 dpi bitmap (it is exactly
     Nyquist) but is easy in a 1200 dpi one.  If the 300 lp/in block resolves
     into stripes on the 1200 sheet and turns to uniform grey on the 600 sheet,
     the mode carries real detail.  If both are mush, it is decorative.

  3. IS IT ASYMMETRIC?  Every grating is drawn twice, once with vertical bars
     and once with horizontal bars.  Vertical bars vary along the cross-feed
     axis (2400 dpi available); horizontal bars vary along the feed axis (600
     dpi available).  If the engine really is 600x2400, the vertical blocks
     should stay crisp to a much finer pitch than the horizontal ones.  Equal
     behaviour in both axes would mean the addressability figures do not
     describe what the raster path actually does.

Usage:  python3 tools/restest.py --dpi 1200 | ./brpdf -r 1200 -p A4 -j res1200
        python3 tools/restest.py --dpi 600  | ./brpdf -r 600  -p A4 -j res600
"""
import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testpage import Bitmap, PAPERS            # noqa: E402

# Line pairs per inch.  600 dpi can represent up to 300 lp/in in principle
# (1 px on, 1 px off) but cannot exceed it; 1200 dpi reaches 600 lp/in.
GRATINGS = [50, 75, 100, 150, 200, 300]


def grating(b, x0, y0, size, lpi, dpi, horizontal):
    """Alternating bars at `lpi` line pairs per inch, clipped to a square."""
    period = dpi / float(lpi)          # device px per black+white pair
    bar = period / 2.0
    n = int(size / period) + 1
    for i in range(n):
        s = y0 + i * period if horizontal else x0 + i * period
        e = s + bar
        if horizontal:
            b.rect(x0, round(s), x0 + size, min(round(e), y0 + size))
        else:
            b.rect(round(s), y0, min(round(e), x0 + size), y0 + size)


def build(paper, dpi, label):
    wmm, hmm = PAPERS[paper]
    mm = dpi / 25.4
    W, H = round(wmm * mm), round(hmm * mm)
    b = Bitmap(W, H)
    line = max(1, round(0.08 * mm))
    lab = max(2, round(0.10 * mm))

    # --- mm rulers on all four edges: scale read-out -------------------------
    for x_mm in range(0, int(wmm) + 1):
        x = round(x_mm * mm)
        ln = 6 * mm if x_mm % 10 == 0 else (3.5 * mm if x_mm % 5 == 0 else 1.8 * mm)
        b.rect(x, 0, x + line, ln)
        b.rect(x, H - ln, x + line, H)
        if x_mm % 20 == 0 and x_mm > 0:
            b.text(x + round(0.7 * mm), round(6.5 * mm), str(x_mm), scale=lab)
    for y_mm in range(0, int(hmm) + 1):
        y = round(y_mm * mm)
        ln = 6 * mm if y_mm % 10 == 0 else (3.5 * mm if y_mm % 5 == 0 else 1.8 * mm)
        b.rect(0, y, ln, y + line)
        b.rect(W - ln, y, W, y + line)
        if y_mm % 20 == 0 and y_mm > 0:
            b.text(round(6.5 * mm), y + round(0.7 * mm), str(y_mm), scale=lab)

    # --- exact 1-inch step blocks: independent scale check ------------------
    ix, iy = round(15 * mm), round(20 * mm)
    for i in range(5):
        b.rect(ix + i * dpi, iy, ix + i * dpi + round(0.5 * mm), iy + round(6 * mm))
    b.rect(ix, iy, ix + 4 * dpi, iy + line)
    b.text(ix, iy + round(8 * mm), "4 TICKS = 4.00 INCH = 101.6MM", scale=lab)

    # --- the gratings -------------------------------------------------------
    size = round(18 * mm)
    gap = round(9 * mm)
    top = round(45 * mm)
    b.text(ix, top - round(8 * mm), "VERTICAL BARS - CROSS FEED AXIS - 2400DPI CLAIMED", scale=lab)
    for i, lpi in enumerate(GRATINGS):
        x = ix + i * (size + gap)
        grating(b, x, top, size, lpi, dpi, horizontal=False)
        b.text(x, top + size + round(2 * mm), "%d LPI" % lpi, scale=lab)

    top2 = top + size + round(18 * mm)
    b.text(ix, top2 - round(8 * mm), "HORIZONTAL BARS - FEED AXIS - 600DPI CLAIMED", scale=lab)
    for i, lpi in enumerate(GRATINGS):
        x = ix + i * (size + gap)
        grating(b, x, top2, size, lpi, dpi, horizontal=True)
        b.text(x, top2 + size + round(2 * mm), "%d LPI" % lpi, scale=lab)

    # --- isolated hairlines: minimum renderable feature ---------------------
    top3 = top2 + size + round(20 * mm)
    b.text(ix, top3 - round(6 * mm), "ISOLATED HAIRLINES 1 2 3 4 6 8 DEVICE PX", scale=lab)
    x = ix
    for w in (1, 2, 3, 4, 6, 8):
        b.rect(x, top3, x + w, top3 + round(20 * mm))              # vertical rule
        b.rect(x, top3 + round(24 * mm), x + round(20 * mm), top3 + round(24 * mm) + w)
        b.text(x, top3 + round(30 * mm), str(w), scale=lab)
        x += round(9 * mm)

    # --- identity -----------------------------------------------------------
    big = max(4, round(0.28 * mm))
    b.text(ix, H - round(40 * mm), label, scale=big)
    b.text(ix, H - round(30 * mm), "%s %dDPI BITMAP %dX%d" % (paper, dpi, W, H), scale=lab)
    return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--paper", default="A4", choices=sorted(PAPERS))
    ap.add_argument("--dpi", type=int, default=1200, choices=(300, 600, 1200))
    ap.add_argument("--label", default=None)
    a = ap.parse_args()
    label = a.label or "RES %d" % a.dpi
    sys.stdout.buffer.write(build(a.paper, a.dpi, label).pbm())


if __name__ == "__main__":
    main()
