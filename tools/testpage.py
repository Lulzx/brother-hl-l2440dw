#!/usr/bin/env python3
"""testpage.py -- generate a calibration sheet for measuring what the engine
actually does with a job, as raw PBM on stdout (feed straight to ./brpdf).

Two questions this page is built to answer with a ruler:

  1. Where does the engine put the first raster pixel?  brlaser's PPD claims an
     8pt/8pt/8pt/16pt (left/right/bottom/top) unprintable margin and brpdf
     crops to match; the printer's own firmware reports 4.233mm = 12pt uniform
     over both SNMP and IPP.  Those cannot both be right.  The corner marks sit
     on the first and last pixel brpdf emits, so measuring paper edge -> mark
     reads the true origin directly.  Rulers along every edge are graduated in
     millimetres from the paper edge for the fine reading.

  2. How must the back side of a duplex sheet be oriented?  brlaser's PPD says
     `Duplex rotated` (rotate the back image 180 degrees); the printer's IPP
     stack says `pwg-raster-document-sheet-back = normal` (do not).  Both pages
     carry the wedge in the SAME corner and a big FRONT/BACK label, so the back
     side's own orientation is the read-out: flip the printed sheet left-to-
     right about its long edge and look at the word BACK.  Upright means the
     180-degree pre-rotation brpdf applies was correct (brlaser's model); upside
     down means the engine wanted the image unrotated (the IPP model); mirrored
     would mean a flip rather than a rotation, which neither model predicts.

Usage:  python3 tools/testpage.py [--paper A4|LETTER] [--dpi 600] > cal.pbm
        python3 tools/testpage.py | ./brpdf -p A4 -j cal > cal.prn
"""
import argparse
import sys

# 5x7 glyphs, one string of five bits per row, top row first.
FONT = {
    " ": ["00000"] * 7,
    "0": ["01110","10001","10011","10101","11001","10001","01110"],
    "1": ["00100","01100","00100","00100","00100","00100","01110"],
    "2": ["01110","10001","00001","00010","00100","01000","11111"],
    "3": ["11111","00010","00100","00010","00001","10001","01110"],
    "4": ["00010","00110","01010","10010","11111","00010","00010"],
    "5": ["11111","10000","11110","00001","00001","10001","01110"],
    "6": ["00110","01000","10000","11110","10001","10001","01110"],
    "7": ["11111","00001","00010","00100","01000","01000","01000"],
    "8": ["01110","10001","10001","01110","10001","10001","01110"],
    "9": ["01110","10001","10001","01111","00001","00010","01100"],
    "A": ["01110","10001","10001","11111","10001","10001","10001"],
    "B": ["11110","10001","10001","11110","10001","10001","11110"],
    "C": ["01110","10001","10000","10000","10000","10001","01110"],
    "D": ["11110","10001","10001","10001","10001","10001","11110"],
    "E": ["11111","10000","10000","11110","10000","10000","11111"],
    "F": ["11111","10000","10000","11110","10000","10000","10000"],
    "G": ["01110","10001","10000","10111","10001","10001","01111"],
    "H": ["10001","10001","10001","11111","10001","10001","10001"],
    "I": ["01110","00100","00100","00100","00100","00100","01110"],
    "J": ["00111","00010","00010","00010","00010","10010","01100"],
    "K": ["10001","10010","10100","11000","10100","10010","10001"],
    "L": ["10000","10000","10000","10000","10000","10000","11111"],
    "M": ["10001","11011","10101","10101","10001","10001","10001"],
    "N": ["10001","11001","10101","10011","10001","10001","10001"],
    "O": ["01110","10001","10001","10001","10001","10001","01110"],
    "P": ["11110","10001","10001","11110","10000","10000","10000"],
    "Q": ["01110","10001","10001","10001","10101","10010","01101"],
    "R": ["11110","10001","10001","11110","10100","10010","10001"],
    "S": ["01111","10000","10000","01110","00001","00001","11110"],
    "T": ["11111","00100","00100","00100","00100","00100","00100"],
    "U": ["10001","10001","10001","10001","10001","10001","01110"],
    "V": ["10001","10001","10001","10001","10001","01010","00100"],
    "W": ["10001","10001","10001","10101","10101","11011","10001"],
    "X": ["10001","10001","01010","00100","01010","10001","10001"],
    "Y": ["10001","10001","01010","00100","00100","00100","00100"],
    "Z": ["11111","00001","00010","00100","01000","10000","11111"],
    ".": ["00000","00000","00000","00000","00000","01100","01100"],
    "/": ["00001","00010","00010","00100","01000","01000","10000"],
    "-": ["00000","00000","00000","11111","00000","00000","00000"],
    ":": ["00000","01100","01100","00000","01100","01100","00000"],
}

PAPERS = {"A4": (210.0, 297.0), "LETTER": (215.9, 279.4), "LEGAL": (215.9, 355.6)}
# brlaser / brpdf model, in points: left, right, bottom, top.
BRLASER_PT = (8, 8, 8, 16)
FIRMWARE_PT = 12.0  # 4.233 mm, uniform, per SNMP prtMarkerMargins and IPP


class Bitmap:
    """1 = black, matching PBM's convention."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def rect(self, x0, y0, x1, y1):
        x0, y0 = max(0, int(x0)), max(0, int(y0))
        x1, y1 = min(self.w, int(x1)), min(self.h, int(y1))
        for y in range(y0, y1):
            row = y * self.w
            for x in range(x0, x1):
                self.px[row + x] = 1

    def text(self, x, y, s, scale=3, vertical=False):
        cx = x
        for ch in s.upper():
            g = FONT.get(ch, FONT[" "])
            for ry, bits in enumerate(g):
                for rx, b in enumerate(bits):
                    if b == "1":
                        if vertical:
                            self.rect(x - (ry + 1) * scale, cx + rx * scale,
                                      x - ry * scale, cx + (rx + 1) * scale)
                        else:
                            self.rect(cx + rx * scale, y + ry * scale,
                                      cx + (rx + 1) * scale, y + (ry + 1) * scale)
            cx += 6 * scale
        return cx

    def pbm(self):
        stride = (self.w + 7) // 8
        out = bytearray(stride * self.h)
        for y in range(self.h):
            base, obase = y * self.w, y * stride
            for x in range(self.w):
                if self.px[base + x]:
                    out[obase + (x >> 3)] |= 0x80 >> (x & 7)
        return b"P4\n%d %d\n" % (self.w, self.h) + bytes(out)


def build(paper, dpi, label, wedge_corner):
    wmm, hmm = PAPERS[paper]
    mm = dpi / 25.4
    pt = dpi / 72.0
    W, H = round(wmm * mm), round(hmm * mm)
    b = Bitmap(W, H)
    line = max(2, round(0.08 * mm))     # ~0.08mm strokes: visible, little toner

    # --- Millimetre rulers referenced to the physical paper edge -------------
    for x_mm in range(0, int(wmm) + 1):
        x = round(x_mm * mm)
        ln = 6 * mm if x_mm % 10 == 0 else (3.5 * mm if x_mm % 5 == 0 else 1.8 * mm)
        b.rect(x, 0, x + line, ln)                    # top edge
        b.rect(x, H - ln, x + line, H)                # bottom edge
        if x_mm % 10 == 0 and x_mm > 0:
            b.text(x + round(0.7 * mm), round(6.5 * mm), str(x_mm), scale=max(2, round(0.09 * mm)))
    for y_mm in range(0, int(hmm) + 1):
        y = round(y_mm * mm)
        ln = 6 * mm if y_mm % 10 == 0 else (3.5 * mm if y_mm % 5 == 0 else 1.8 * mm)
        b.rect(0, y, ln, y + line)                    # left edge
        b.rect(W - ln, y, W, y + line)                # right edge
        if y_mm % 10 == 0 and y_mm > 0:
            b.text(round(6.5 * mm), y + round(0.7 * mm), str(y_mm), scale=max(2, round(0.09 * mm)))

    # --- The two competing printable-area frames ----------------------------
    def frame(l, r, bot, top, dash):
        x0, x1 = round(l * pt), W - round(r * pt)
        y0, y1 = round(top * pt), H - round(bot * pt)
        for x in range(x0, x1, dash * 2):
            b.rect(x, y0, min(x + dash, x1), y0 + line)
            b.rect(x, y1 - line, min(x + dash, x1), y1)
        for y in range(y0, y1, dash * 2):
            b.rect(x0, y, x0 + line, min(y + dash, y1))
            b.rect(x1 - line, y, x1, min(y + dash, y1))
        return x0, y0

    ax, ay = frame(*BRLASER_PT, dash=round(2.0 * mm))                 # hypothesis A
    bx, by = frame(*([FIRMWARE_PT] * 4), dash=round(0.6 * mm))        # hypothesis B

    # Solid L-marks sitting on hypothesis A's corner -- this is the first pixel
    # brpdf actually emits, so paper-edge -> L is the number to measure.
    arm, thick = round(12 * mm), round(0.5 * mm)
    for (mx, my, sx, sy) in ((ax, ay, 1, 1), (W - round(8 * pt), ay, -1, 1),
                             (ax, H - round(8 * pt), 1, -1),
                             (W - round(8 * pt), H - round(8 * pt), -1, -1)):
        b.rect(min(mx, mx + sx * arm), min(my, my + sy * thick),
               max(mx, mx + sx * arm), max(my, my + sy * thick))
        b.rect(min(mx, mx + sx * thick), min(my, my + sy * arm),
               max(mx, mx + sx * thick), max(my, my + sy * arm))

    lab = max(3, round(0.13 * mm))
    b.text(round(20 * mm), round(16 * mm), "A SOLID L - BRLASER 8PT/16PT", scale=lab)
    b.text(round(20 * mm), round(22 * mm), "B FINE DASH - FIRMWARE 12PT", scale=lab)
    b.text(round(20 * mm), round(30 * mm), "MEASURE PAPER EDGE TO SOLID L", scale=lab)

    # --- Solid wedge, held well inboard so it never covers a corner mark ----
    wsz = round(14 * mm)
    wx, wy = (round(45 * mm), round(45 * mm)) if wedge_corner == "tl" else \
             (W - round(45 * mm) - wsz, H - round(45 * mm) - wsz)
    for i in range(wsz):
        if wedge_corner == "tl":
            b.rect(wx, wy + i, wx + wsz - i, wy + i + 1)
        else:
            b.rect(wx + i, wy + wsz - i - 1, wx + wsz, wy + wsz - i)

    # --- Centre crosshair and identity ---------------------------------------
    cx, cy, arm = W // 2, H // 2, round(10 * mm)
    b.rect(cx - arm, cy - line, cx + arm, cy + line)
    b.rect(cx - line, cy - arm, cx + line, cy + arm)
    big = max(4, round(0.30 * mm))
    b.text(cx - round(len(label) * 3 * 6 * big / 6), cy + round(14 * mm), label, scale=big)
    b.text(cx - round(18 * mm), cy - round(24 * mm), f"{paper} {dpi}DPI", scale=max(3, round(0.14 * mm)))
    return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--paper", default="A4", choices=sorted(PAPERS))
    ap.add_argument("--dpi", type=int, default=600)
    ap.add_argument("--pages", default="FRONT,BACK",
                    help="comma-separated page labels (2 pages exercises duplex)")
    a = ap.parse_args()
    out = sys.stdout.buffer
    for i, label in enumerate(a.pages.split(",")):
        out.write(build(a.paper, a.dpi, label.strip(), "tl").pbm())


if __name__ == "__main__":
    main()
