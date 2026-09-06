#!/usr/bin/env python3
"""probe.py -- interrogate a real Brother HL-L2440DW over the network.

Everything the printer will tell you about itself without printing a page:
PJL identity and page count over tcp/9100, the Printer MIB counters and
engine geometry over SNMP, and the IPP capability set over tcp/631.

Talking to this device's PJL back-channel has three non-obvious rules, learned
the hard way and encoded below:

  1. Never half-close the socket.  A client that does shutdown(SHUT_WR) after
     sending a query gets zero bytes back.  This is why `nc` makes the printer
     look like it has no back-channel at all -- it does.
  2. Only one connection to 9100 at a time, and the port stays busy for a
     second or two after close.  Batch queries into one connection.
  3. Replies can lag: a query's answer may arrive on a *later* connection.
     Every reply echoes the command it answers, so match on the echo rather
     than assuming request/response order.

Usage:  python3 tools/probe.py [ip]
"""
import re
import socket
import subprocess
import sys
import time

UEL = b"\x1b%-12345X"
DEFAULT_IP = os.environ.get("BRPRINTER", "")   # set BRPRINTER, or pass an address

# Printer MIB.  Margins come back in micrometres; the IPP path reports the
# same numbers in hundredths of a millimetre.
OIDS = {
    "sysDescr":        "1.3.6.1.2.1.1.1.0",
    "status":          "1.3.6.1.2.1.25.3.5.1.1.1",
    "life_count":      "1.3.6.1.2.1.43.10.2.1.4.1.1",
    "since_power_on":  "1.3.6.1.2.1.43.10.2.1.5.1.1",  # impressions since power-on, not a power-cycle count
    "dpi_feed":        "1.3.6.1.2.1.43.10.2.1.9.1.1",
    "dpi_xfeed":       "1.3.6.1.2.1.43.10.2.1.10.1.1",
    "margin_north_um": "1.3.6.1.2.1.43.10.2.1.11.1.1",
    "margin_south_um": "1.3.6.1.2.1.43.10.2.1.12.1.1",
    "margin_west_um":  "1.3.6.1.2.1.43.10.2.1.13.1.1",
    "margin_east_um":  "1.3.6.1.2.1.43.10.2.1.14.1.1",
    "toner_max":       "1.3.6.1.2.1.43.11.1.1.8.1.1",
    "toner_level":     "1.3.6.1.2.1.43.11.1.1.9.1.1",
    "drum_max":        "1.3.6.1.2.1.43.11.1.1.8.1.2",
    "drum_level":      "1.3.6.1.2.1.43.11.1.1.9.1.2",
}


def pjl(ip, commands, read_secs=20, connect_tries=30):
    """Send PJL commands on one connection; return {command_arg: reply}."""
    sock = None
    for _ in range(connect_tries):
        try:
            sock = socket.create_connection((ip, 9100), timeout=5)
            break
        except OSError:
            time.sleep(1.5)
    if sock is None:
        return {}
    body = b"".join(c.encode() + b"\r\n" for c in commands)
    buf = b""
    try:
        sock.sendall(UEL + body)          # note: no shutdown() -- see rule 1
        sock.settimeout(read_secs)
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    finally:
        sock.close()
    out = {}
    for m in re.finditer(r"@PJL (?:INFO|INQUIRE|DINQUIRE|ECHO) (\w+)\r?\n([^\x0c]*)\x0c",
                         buf.decode("latin-1")):
        out[m.group(1)] = m.group(2).strip()
    return out


def snmp(ip):
    """Map replies back by OID -- zipping them positionally silently corrupts
    every field if the agent drops or reorders one."""
    try:
        out = subprocess.run(
            ["snmpget", "-v1", "-c", "public", "-OQn", "-t", "3", ip] + list(OIDS.values()),
            capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    by_oid = {}
    for line in out.splitlines():
        if "=" not in line:
            continue
        oid, _, val = line.partition("=")
        by_oid[oid.strip().lstrip(".")] = val.strip().strip('"')
    return {name: by_oid[oid] for name, oid in OIDS.items() if oid in by_oid}


def ipp(ip):
    try:
        out = subprocess.run(
            ["ipptool", "-tv", f"ipp://{ip}/ipp/print", "get-printer-attributes.test"],
            capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    return {m.group(1): m.group(2).strip()
            for m in re.finditer(r"^\s+([a-z0-9-]+) \([^)]+\) = (.*)$", out, re.M)}


def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IP
    print(f"== {ip}\n")

    info = pjl(ip, ["@PJL INFO ID", "@PJL INFO STATUS", "@PJL INFO PAGECOUNT"])
    print("-- PJL (tcp/9100)")
    for k in ("ID", "STATUS", "PAGECOUNT"):
        for line in (info.get(k) or "<no reply>").splitlines():
            print(f"   {k:10s} {line.strip()}")

    s = snmp(ip)
    if s and all(k in s for k in ("sysDescr", "life_count", "margin_north_um")):
        print("\n-- SNMP (udp/161)")
        print(f"   {'device':10s} {s['sysDescr']}")
        print(f"   {'state':10s} {s['status']}")
        print(f"   {'printed':10s} {s['life_count']} impressions lifetime, "
              f"{s['since_power_on']} since power-on")
        print(f"   {'engine':10s} {s['dpi_feed']} dpi feed x {s['dpi_xfeed']} dpi cross-feed")
        um = [int(s[f"margin_{d}_um"]) for d in ("north", "south", "west", "east")]
        print(f"   {'margins':10s} " + ", ".join(
            f"{d} {v/1000:.2f}mm ({v/1000/25.4*72:.1f}pt)"
            for d, v in zip(("top", "bottom", "left", "right"), um)))
        for name, lvl, mx in (("toner", s["toner_level"], s["toner_max"]),
                              ("drum", s["drum_level"], s["drum_max"])):
            pct = f"{int(lvl)/int(mx)*100:.0f}%" if int(mx) > 0 else "unquantified"
            print(f"   {name:10s} {pct} ({lvl}/{mx})")

    a = ipp(ip)
    if a:
        print("\n-- IPP (tcp/631)")
        for k in ("printer-make-and-model", "printer-device-id", "printer-state",
                  "printer-state-reasons", "printer-resolution-supported",
                  "sides-supported", "document-format-supported",
                  "pwg-raster-document-sheet-back", "media-default",
                  "media-top-margin-supported", "media-left-margin-supported"):
            if k in a:
                print(f"   {k:34s} {a[k]}")


if __name__ == "__main__":
    main()
