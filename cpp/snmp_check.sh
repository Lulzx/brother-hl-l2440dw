#!/bin/sh
# The SNMP request encoder must be byte-identical to net-snmp's. No printer
# involved: snmpget is aimed at a local UDP socket that captures the packet.
set -eu
cd "$(dirname "$0")"
command -v snmpget >/dev/null 2>&1 || { echo "  skip  snmpget not installed"; exit 0; }
[ -x ./snmp_encode_test ] || make snmp_encode_test >/dev/null
exec python3 - <<'PY'
import socket, subprocess, threading, binascii, sys
OIDS = ["1.3.6.1.2.1.25.3.5.1.1.1", "1.3.6.1.2.1.25.3.5.1.2.1",
        "1.3.6.1.2.1.43.10.2.1.4.1.1", "1.3.6.1.2.1.43.11.1.1.9.1.1"]
srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
srv.bind(("127.0.0.1", 1161)); srv.settimeout(4)
ok = fail = 0
for oid in OIDS:
    threading.Thread(target=lambda: subprocess.run(
        ["snmpget","-v1","-c","public","-t","1","-r","0","127.0.0.1:1161",oid],
        capture_output=True), daemon=True).start()
    try: data,_ = srv.recvfrom(4096)
    except socket.timeout: print("  FAIL  timeout for", oid); fail+=1; continue
    i = data.index(0xA0); j = i+2
    rid = int.from_bytes(data[j+2:j+2+data[j+1]], "big", signed=True)
    ours = subprocess.run(["./snmp_encode_test","public",oid,str(rid)],
                          capture_output=True,text=True).stdout.strip()
    if ours == binascii.hexlify(data).decode():
        print("  ok    %-30s byte-identical to net-snmp" % oid); ok+=1
    else:
        print("  FAIL  %s" % oid); fail+=1
srv.close()
print("snmp encoder: %d/%d byte-identical" % (ok, ok+fail))
sys.exit(1 if fail else 0)
PY
