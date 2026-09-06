#!/bin/sh
# Cancellation semantics, judged by the printer model rather than by assertion.
#
# There is no back-channel and no PJL cancel command on port 9100, so a job can
# only be abandoned, not recalled. What Abort() must guarantee:
#
#   * pages already terminated by a form feed stay committed;
#   * the page in flight is NOT ejected;
#   * the device is left in a clean PJL state, not inside an open escape.
#
# The third is the one that matters. A stream that just stops leaves the parser
# inside the mode-1030 escape sequence, still reading <digits><letter> pairs --
# the state in which stray bytes get interpreted as commands.
set -eu
cd "$(dirname "$0")"
[ -x ./cancel_test ] && : || make cancel_test >/dev/null
./cancel_test >/dev/null

fail=0
check() {                       # file, expected pages, expected warnings, desc
  out=$(python3 ../brsim.py decode "$1" -o /tmp/cc_out 2>&1 | tail -1)
  p=$(printf '%s' "$out" | sed -n 's/.*: \([0-9]*\) page(s).*/\1/p')
  w=$(printf '%s' "$out" | sed -n 's/.*, \([0-9]*\) warning(s).*/\1/p')
  if [ "$p" = "$2" ] && [ "$w" = "$3" ]; then
    echo "  ok    $4  ($p page(s), $w warning(s))"
  else
    echo "  FAIL  $4  expected $2 page/$3 warn, got $p page/$w warn"
    fail=$((fail+1))
  fi
}

check /tmp/clean.prn     3 0 "control: three pages, no cancel"
check /tmp/aborted.prn   1 0 "cancel mid page 2 + Abort(): page 2 abandoned, clean"
check /tmp/truncated.prn 1 1 "naive stop, no Abort(): leaves an open escape"

echo "cancel: $((3-fail))/3 ok"
[ "$fail" = 0 ]
