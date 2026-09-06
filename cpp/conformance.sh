#!/bin/sh
# Every case must be byte-identical to brpdf, which is itself byte-identical to
# brlaser. That chain is the only thing that makes the C++ encoder trustworthy.
set -eu
cd "$(dirname "$0")/.."
[ -x ./brpdf ] || make brpdf >/dev/null
[ -x ./cpp/brhbp_tool ] || make -C cpp >/dev/null

pass=0; fail=0
run() {
  desc=$1; shift
  ./brpdf "$@" < /tmp/conf.pbm > /tmp/c_ref.prn 2>/dev/null
  ./cpp/brhbp_tool "$@" < /tmp/conf.pbm > /tmp/c_new.prn 2>/dev/null
  if cmp -s /tmp/c_ref.prn /tmp/c_new.prn; then
    echo "  ok    $desc"; pass=$((pass+1))
  else
    echo "  FAIL  $desc"; fail=$((fail+1))
  fi
}

python3 tools/testpage.py --paper A4 --dpi 600 --pages P1 > /tmp/conf.pbm 2>/dev/null
run "A4 600 default"      -p A4 -r 600 -j t
run "A4 600 toner save"   -p A4 -r 600 -j t -e
run "A4 600 copies=3"     -p A4 -r 600 -j t -c 3
run "Letter 600"          -p LETTER -r 600 -j t

python3 tools/testpage.py --paper A4 --dpi 600 --pages P1,P2 > /tmp/conf.pbm 2>/dev/null
run "2 pages"             -p A4 -r 600 -j t
run "2 pages duplex"      -p A4 -r 600 -j t -d
run "2 pages duplex -R"   -p A4 -r 600 -j t -d -R

python3 tools/restest.py --dpi 1200 > /tmp/conf.pbm 2>/dev/null
run "A4 1200"             -p A4 -r 1200 -j t
python3 tools/restest.py --dpi 300 > /tmp/conf.pbm 2>/dev/null
run "A4 300"              -p A4 -r 300 -j t

if command -v mutool >/dev/null 2>&1; then
  mutool draw -F pbm -r 600 -o /tmp/conf.pbm test/noise.pdf 2>/dev/null
  run "photographic 600"  -p LETTER -r 600 -j t
  mutool draw -F pbm -r 600 -o /tmp/conf.pbm test/sample.pdf 2>/dev/null
  run "3-page text 600"   -p LETTER -r 600 -j t
fi

echo "conformance: $pass passed, $fail failed"
[ "$fail" = 0 ]
