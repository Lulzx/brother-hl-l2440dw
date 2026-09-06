#!/bin/sh
# The duplex back-page rotation lives in the PDF band renderer, which the
# byte-for-byte conformance suite never touches -- it only drives brhbp_tool
# on a pre-rendered bitmap. That gap let brhbp_pdf ship without the rotation
# at all, printing every second page upside down. This checks the geometry the
# renderer actually produces.
set -eu
cd "$(dirname "$0")/.."
command -v mutool >/dev/null 2>&1 || { echo "  skip  mutool not installed"; exit 0; }
[ -x ./cpp/brhbp_pdf ] || { echo "  skip  brhbp_pdf not built (needs MuPDF)"; exit 0; }

fail=0
for mode in long short; do
  opt=$([ "$mode" = long ] && echo -d || echo -D)
  ./cpp/brhbp_pdf -p LETTER -r 300 $opt -1 -t -q -j dx test/sample.pdf > /tmp/dx.prn
  python3 brsim.py decode /tmp/dx.prn -o /tmp/simdx >/dev/null 2>&1
  python3 - "$mode" <<'PY' || fail=$((fail+1))
import sys, glob, subprocess
from PIL import Image
mode = sys.argv[1]
DPI = 300
subprocess.run(["mutool","draw","-F","pgm","-r",str(DPI),"-o","/tmp/f2.pgm",
                "test/sample.pdf","2"], stderr=subprocess.DEVNULL)
full = Image.open("/tmp/f2.pgm").convert("L")
ml, mt = 8*DPI//72, 16*DPI//72
pw, ph = (612-16)*DPI//72, (792-24)*DPI//72
win = lambda im: im.crop((ml,mt,ml+pw,mt+ph)).point(lambda v: 0 if v<128 else 255)
want = win(full.rotate(180)) if mode == "long" else win(full)
m = [x for x in glob.glob("/tmp/simdx/page-002-*.png") if "preview" not in x]
got = win(Image.open(m[0]).convert("L"))
bad = sum(1 for a,b in zip(want.tobytes(), got.tobytes()) if a != b)
pct = 100.0*bad/(pw*ph)
ok = pct < 0.5
print("  %s  %s-edge back page %s (%.3f%% off model)"
      % ("ok   " if ok else "FAIL ", mode,
         "rotated 180" if mode=="long" else "not rotated", pct))
sys.exit(0 if ok else 1)
PY
done
echo "duplex: $((2-fail))/2 ok"
[ "$fail" = 0 ]
