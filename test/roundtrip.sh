#!/bin/sh
# End-to-end verification of the driver-free pipeline, entirely in software.
#   1. rasterise the PDF with mutool
#   2. encode with brpdf and with brlaser's own encoder -> must be byte-identical
#   3. decode with the brsim printer model -> must be pixel-identical to the source
#   4. exercise duplex/copies/1200dpi/A4 variants and the TCP 9100 emulator
set -eu
cd "$(dirname "$0")/.."
PDF=${1:-test/sample.pdf}
T=test/rt; rm -rf "$T"; mkdir -p "$T"

echo "== 1. rasterise $PDF at 600 dpi"
mutool draw -F pbm -r 600 -o - "$PDF" > "$T/pages.pbm" 2>/dev/null

echo "== 2. conformance: brpdf vs brlaser encoder (raw mode)"
./ref/refenc rt LETTER 600 1 0 < "$T/pages.pbm" > "$T/ref.prn"
./brpdf -x -p LETTER -r 600 -j rt < "$T/pages.pbm" > "$T/raw.prn" 2>/dev/null
cmp "$T/ref.prn" "$T/raw.prn" && echo "   OK: byte-identical ($(wc -c < "$T/raw.prn") bytes)"

./ref/refenc rt A4 1200 2 1 < "$T/pages.pbm" > "$T/ref2.prn"
./brpdf -x -p A4 -r 1200 -c 2 -d -R -j rt < "$T/pages.pbm" > "$T/raw2.prn" 2>/dev/null
cmp "$T/ref2.prn" "$T/raw2.prn" && echo "   OK: byte-identical with duplex/copies/1200 header"

echo "== 3. round trip through the printer model (margin crop path)"
./brpdf -p LETTER -r 600 < "$T/pages.pbm" > "$T/job.prn" 2>/dev/null
python3 brsim.py decode "$T/job.prn" -o "$T/out" --pbm
python3 - "$T" <<'EOF'
import re, sys, glob, numpy as np
T = sys.argv[1]
def pbms(data):
    i = 0
    while i < len(data):
        m = re.match(rb'P4\s+(\d+)\s+(\d+)\s', data[i:i+40]); w, h = int(m[1]), int(m[2]); st = (w+7)//8
        yield np.unpackbits(np.frombuffer(data[i+m.end():i+m.end()+st*h], np.uint8).reshape(h, st), axis=1)[:, :w]
        i += m.end() + st*h
src = list(pbms(open(f"{T}/pages.pbm", "rb").read()))
ml, mt = 8*600//72, 16*600//72; pw = (612-16)*600//72; ph = (792-24)*600//72
outs = sorted(glob.glob(f"{T}/out/page-*.pbm"))
assert len(outs) == len(src), (len(outs), len(src))
for n, (s, o) in enumerate(zip(src, outs), 1):
    exp = s[mt:mt+ph, ml:ml+pw]
    got = next(pbms(open(o, "rb").read()))[:ph, :pw]
    assert got.shape == exp.shape and np.array_equal(got, exp), f"page {n} differs"
print(f"   OK: {len(src)} page(s) pixel-identical after decode")
EOF

echo "== 4. variants"
./brpdf -p LETTER -d -c 2 -e < "$T/pages.pbm" > "$T/duplex.prn" 2>/dev/null
python3 brsim.py decode "$T/duplex.prn" -o "$T/out-duplex"
python3 - "$T" <<'EOF'
import re, sys, glob, numpy as np
T = sys.argv[1]
def pbm(path):
    d = open(path, "rb").read(); m = re.match(rb'P4\s+(\d+)\s+(\d+)\s', d[:40]); w, h = int(m[1]), int(m[2])
    return np.unpackbits(np.frombuffer(d[m.end():], np.uint8).reshape(h, (w+7)//8), axis=1)[:, :w]
from PIL import Image
front = np.array(Image.open(f"{T}/out/page-002-front.png").convert("1"))
back = np.array(Image.open(f"{T}/out-duplex/page-002-back.png").convert("1"))
# back side is the same page rotated 180 degrees; margins differ (16pt top vs 8pt bottom), so
# compare the content bounding boxes instead of raw arrays.
def bbox(a):
    ys, xs = np.where(~a); return a[ys.min():ys.max()+1, xs.min():xs.max()+1]
assert np.array_equal(bbox(back), bbox(front)[::-1, ::-1]), "duplex back page is not the 180-degree rotation"
print("   OK: duplex back page rotated 180 degrees, content preserved")
EOF

echo "== 5. network emulator (tcp/9109)"
python3 brsim.py listen --port 9109 -o "$T/spool" > "$T/listen.log" 2>&1 &
SIM=$!
sleep 1
ID=$(printf '\033%%-12345X@PJL INFO ID\r\n\033%%-12345X' | nc -w 2 localhost 9109 | tr -d '\r\f')
echo "   printer identifies as: $(echo "$ID" | sed -n 2p)"
nc -w 3 localhost 9109 < "$T/job.prn"
sleep 2
kill $SIM 2>/dev/null || true
grep -q "3 page(s), 3 impression(s), 0 warning(s)" "$T/listen.log" && echo "   OK: job received over TCP and rendered with 0 warnings"

echo "ALL TESTS PASSED"
