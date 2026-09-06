#!/usr/bin/env python3
"""
brsim.py -- software model of a Brother HL-L2440DW (HL-L2400 series) print engine.

It consumes the exact byte stream a host would send to the printer
(PJL envelope + PCL-flavoured page header + mode-1030 raster bands) and
does what the printer would do: track PJL environment variables, honour
the PCL reset/copies/duplex commands, decode every raster band, and lay
the bitmap onto a sheet of the selected paper at the engine's unprintable
margins.  Each sheet side is written out as a PNG so a job can be checked
without touching hardware.

    brsim.py decode job.prn -o out/            # render a captured/generated job
    brsim.py listen --port 9100 -o spool/      # emulate the printer on the network
    brsim.py decode job.prn --pbm out/         # also dump raw decoded bitmaps

Only the standard library is required for parsing; Pillow + numpy are used
for PNG output (falls back to PBM if missing).
"""
import argparse
import os
import re
import socketserver
import sys
import time

MODEL = "Brother HL-L2440DW series"
DEVICE_ID = ("MFG:Brother;CMD:PJL,HBP,URF;MDL:HL-L2440DW series;CLS:PRINTER;"
             "CID:Brother Laser Type1;")

# Paper table: name -> (width pt, height pt).  Same set brpdf/brlaser use.
PAPERS = {
    "A4": (595, 842), "LETTER": (612, 792), "LEGAL": (612, 1008),
    "A5": (420, 595), "A6": (298, 420), "B5": (516, 729), "B6": (363, 516),
    "EXECUTIVE": (522, 756), "FOLIO": (612, 936), "C5": (459, 649),
    "DL": (312, 624), "MONARCH": (279, 540),
}
MARGIN_L, MARGIN_T = 8, 16          # points; engine's unprintable border
MAX_LINE_BYTES = 2000                # ~16000 px; anything longer is garbage


class StreamError(Exception):
    pass


# --------------------------------------------------------------------------
# Mode-1030 band decoder (line-delta compression)

class RasterDecoder:
    """Decodes Brother mode-1030 bands into a list of rows (bytes)."""

    def __init__(self, log):
        self.log = log
        self.line = bytearray()      # persists: reference for the next row
        self.rows = []

    def decode_band(self, data):
        if len(data) < 2:
            raise StreamError("band shorter than 2 bytes")
        nlines = (data[0] << 8) | data[1]
        pos = 2
        for i in range(nlines):
            pos = self._decode_line(data, pos, first_in_band=(i == 0))
        if pos != len(data):
            self.log(f"warning: band has {len(data) - pos} trailing byte(s)")
        return nlines

    def _overflow(self, data, pos):
        total = 0
        while True:
            if pos >= len(data):
                raise StreamError("truncated overflow field")
            b = data[pos]; pos += 1
            total += b
            if b != 255:
                return total, pos

    def _decode_line(self, data, pos, first_in_band):
        if pos >= len(data):
            raise StreamError("truncated band (missing edit count)")
        nedits = data[pos]; pos += 1
        if nedits == 0xFF:                       # blank row
            self.line = bytearray()
            self.rows.append(bytes())
            return pos
        off = 0
        self_contained = True
        for e in range(nedits):
            if pos >= len(data):
                raise StreamError("truncated band (missing edit)")
            cmd = data[pos]; pos += 1
            if cmd & 0x80:                       # repeat
                skip = (cmd >> 5) & 3
                if skip == 3:
                    x, pos = self._overflow(data, pos); skip += x
                cnt = cmd & 31
                if cnt == 31:
                    x, pos = self._overflow(data, pos); cnt += x
                cnt += 2
                if pos >= len(data):
                    raise StreamError("truncated repeat value")
                val = data[pos]; pos += 1
                payload = bytes([val]) * cnt
            else:                                # substitute (literal bytes)
                skip = (cmd >> 3) & 15
                if skip == 15:
                    x, pos = self._overflow(data, pos); skip += x
                cnt = cmd & 7
                if cnt == 7:
                    x, pos = self._overflow(data, pos); cnt += x
                cnt += 1
                if pos + cnt > len(data):
                    raise StreamError("truncated substitute data")
                payload = data[pos:pos + cnt]; pos += cnt
            if skip:
                self_contained = False
            end = off + skip + cnt
            if end > MAX_LINE_BYTES:
                raise StreamError(f"row longer than {MAX_LINE_BYTES} bytes")
            if end > len(self.line):
                self.line.extend(b"\0" * (end - len(self.line)))
            off += skip
            self.line[off:off + cnt] = payload
            off += cnt
        # A row that starts a band and still relies on bytes carried over from
        # the previous band (skips, or edits that stop short of the row end)
        # is fragile: Brother's own driver restarts every band self-contained.
        if first_in_band and (not self_contained or nedits != 1 or off != len(self.line)):
            self.log("warning: first row of a band depends on previous band state")
        self.rows.append(bytes(self.line))
        return pos


# --------------------------------------------------------------------------
# Job parser: PJL + PCL escape sequences

class Page:
    def __init__(self, env, copies, duplex, rows):
        self.env, self.copies, self.duplex, self.rows = dict(env), copies, duplex, rows


class Printer:
    UEL = b"\x1b%-12345X"

    def __init__(self, log=lambda s: None):
        self.log = log
        self.env = {}            # PJL SET variables
        self.jobname = None
        self.pages = []
        self.copies = 1
        self.duplex = False
        self.compression = None
        self.dec = RasterDecoder(log)
        self.stray = 0
        self.pjl_responses = []
        self.warnings = []

    def warn(self, msg):
        self.warnings.append(msg); self.log(msg)

    # ---- PJL -------------------------------------------------------------
    def pjl_line(self, line):
        text = line.decode("latin-1").strip()
        if not text.startswith("@PJL"):
            if text:
                self.warn(f"warning: non-PJL text in PJL mode: {text!r}")
            return False
        body = text[4:].strip()
        m = re.match(r"(\w+)\s*(.*)", body)
        if not m:
            return False
        cmd, rest = m.group(1).upper(), m.group(2)
        if cmd == "JOB":
            mm = re.search(r'NAME\s*=\s*"([^"]*)"', rest)
            self.jobname = mm.group(1) if mm else ""
            self.log(f"PJL JOB {self.jobname!r}")
        elif cmd == "EOJ":
            self.log("PJL EOJ")
        elif cmd in ("SET", "DEFAULT"):
            mm = re.match(r"(\w+)\s*=\s*(.*)", rest)
            if mm:
                self.env[mm.group(1).upper()] = mm.group(2).strip()
                self.log(f"PJL {cmd} {mm.group(1).upper()} = {mm.group(2).strip()}")
        elif cmd == "ENTER":
            lang = rest.upper().replace(" ", "")
            self.log(f"PJL ENTER {lang}")
            if lang != "LANGUAGE=PCL":
                self.warn(f"warning: unsupported language {rest!r} (HL-L2440DW only speaks the host-based raster)")
            return True                       # switch to PCL
        elif cmd in ("INFO", "INQUIRE", "DINQUIRE", "ECHO", "USTATUS", "USTATUSOFF"):
            self.pjl_responses.append(self.pjl_reply(cmd, rest))
        elif cmd in ("COMMENT", "RDYMSG", "OPMSG", "STMSG", "INITIALIZE", "RESET"):
            self.log(f"PJL {cmd} {rest}")
        else:
            self.warn(f"warning: unknown PJL command {cmd}")
        return False

    def pjl_reply(self, cmd, rest):
        arg = rest.strip().upper()
        if cmd == "INFO" and arg == "ID":
            return f'@PJL INFO ID\r\n"{MODEL}"\r\n\x0c'
        if cmd == "INFO" and arg == "STATUS":
            return "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\nONLINE=TRUE\r\n\x0c"
        if cmd == "ECHO":
            return f"@PJL ECHO {rest}\r\n\x0c"
        if cmd == "INQUIRE":
            return f"@PJL INQUIRE {arg}\r\n{self.env.get(arg, '?')}\r\n\x0c"
        return f"@PJL {cmd} {rest}\r\n?\r\n\x0c"

    # ---- PCL -------------------------------------------------------------
    def reset(self):
        self.copies, self.duplex, self.compression = 1, False, None
        self.dec = RasterDecoder(self.log)

    def eject(self):
        rows = self.dec.rows
        if not rows:
            self.log("form feed with no raster (blank page suppressed)")
        else:
            self.pages.append(Page(self.env, self.copies, self.duplex, rows))
            self.log(f"page {len(self.pages)}: {len(rows)} rows, "
                     f"max {max(len(r) for r in rows)} bytes/row")
        self.dec = RasterDecoder(self.log)

    def pcl_param(self, group, value, letter):
        key = (group, letter)
        if key == (b"&l", "X"):
            self.copies = max(1, int(value)); self.log(f"PCL copies={self.copies}")
        elif key == (b"&l", "S"):
            self.duplex = int(value) != 0; self.log(f"PCL duplex={int(value)} ({'long edge' if int(value)==2 else 'short edge' if int(value)==1 else 'off'})")
        elif key == (b"&l", "A"):
            self.log(f"PCL page size code {value}")
        elif key == (b"*b", "M"):
            self.compression = int(value); self.log(f"PCL raster compression mode {self.compression}")
        elif key == (b"*r", "A"):
            self.log("PCL start raster")
        elif key == (b"*r", "C") or key == (b"*r", "B"):
            self.log("PCL end raster")
        else:
            self.warn(f"warning: unhandled PCL command ESC{group.decode()}{value}{letter}")

    def pcl_raster(self, data):
        if self.compression != 1030:
            self.warn(f"warning: raster data with compression {self.compression}; "
                      "this engine only accepts mode 1030")
            return
        n = self.dec.decode_band(data)
        self.log(f"  band: {len(data)} bytes, {n} rows")

    # ---- Byte stream driver ---------------------------------------------
    def feed(self, data):
        pos, n = 0, len(data)
        mode = "pcl"     # cold printer: PCL-ish until a UEL says otherwise
        while pos < n:
            b = data[pos]
            if data.startswith(self.UEL, pos):
                pos += len(self.UEL)
                mode = "pjl"
                # UEL may be immediately followed by "@PJL" on the same line
                continue
            if mode == "pjl":
                eol = data.find(b"\n", pos)
                if eol < 0:
                    eol = n
                line = data[pos:eol]
                pos = eol + 1
                if line.startswith(b"\x1b"):     # PCL sneaking in without ENTER
                    pos -= len(line) + 1; mode = "pcl"; continue
                if self.pjl_line(line):
                    mode = "pcl"
                continue
            # PCL mode
            if b == 0x1b:
                pos = self.pcl_escape(data, pos)
            elif b == 0x0c:
                self.eject(); pos += 1
            elif b in (0x00, 0x0a, 0x0d):
                pos += 1                              # padding / line ends
            else:
                self.stray += 1; pos += 1
        if self.stray:
            self.warn(f"warning: {self.stray} stray non-command byte(s) ignored in PCL mode")

    def pcl_escape(self, data, pos):
        n = len(data)
        if pos + 1 >= n:
            raise StreamError("truncated escape")
        c = data[pos + 1]
        if c == ord("E"):
            self.log("PCL reset (ESC E)"); self.reset(); return pos + 2
        if c in b"&*(:)%":
            if pos + 2 >= n:
                raise StreamError("truncated escape")
            group = bytes([c, data[pos + 2]])
            p = pos + 3
            while True:
                m = re.compile(rb"[+-]?\d*\.?\d*").match(data, p)
                value = m.group(0).decode() if m else ""
                p = m.end()
                if p >= n:
                    raise StreamError("truncated parameterised escape")
                letter = chr(data[p]); p += 1
                if group == b"*b" and letter in "wW":
                    cnt = int(value or 0)
                    if p + cnt > n:
                        raise StreamError("truncated raster band")
                    self.pcl_raster(data[p:p + cnt]); p += cnt
                else:
                    self.pcl_param(group, value or "0", letter.upper())
                if letter.isupper():
                    return p
            # unreachable
        self.warn(f"warning: unknown two-char escape ESC {chr(c)!r}")
        return pos + 2


# --------------------------------------------------------------------------
# Sheet composition

def page_geometry(page):
    env = page.env
    dpi = int(env.get("RESOLUTION", "600"))
    if env.get("RAS1200MODE", "FALSE").upper() == "TRUE":
        dpi = 1200
    paper = env.get("PAPER", "A4").upper()
    w_pt, h_pt = PAPERS.get(paper, PAPERS["A4"])
    return dpi, paper, w_pt * dpi // 72, h_pt * dpi // 72, MARGIN_L * dpi // 72, MARGIN_T * dpi // 72


def render(page, index, outdir, want_pbm, log):
    dpi, paper, pw, ph, ox, oy = page_geometry(page)
    stride = max((len(r) for r in page.rows), default=0)
    raster_w, raster_h = stride * 8, len(page.rows)
    clipped = raster_w > pw - ox or raster_h > ph - oy
    if clipped:
        log(f"warning: page {index} raster {raster_w}x{raster_h} exceeds printable area of {paper}@{dpi}; clipped")
    side = "back" if (page.duplex and index % 2 == 0) else "front"
    base = os.path.join(outdir, f"page-{index:03d}-{side}")

    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        np = Image = None

    if np is not None:
        raw = np.zeros((raster_h, stride), dtype=np.uint8)
        for y, r in enumerate(page.rows):
            if r:
                raw[y, :len(r)] = np.frombuffer(r, dtype=np.uint8)
        bits = np.unpackbits(raw, axis=1)                     # 1 = toner
        sheet = np.zeros((ph, pw), dtype=np.uint8)
        h = min(raster_h, ph - oy); w = min(raster_w, pw - ox)
        sheet[oy:oy + h, ox:ox + w] = bits[:h, :w]
        img = Image.fromarray((1 - sheet) * 255).convert("1")
        img.save(base + ".png", optimize=True)
        prev = img.convert("L").resize((pw * 100 // dpi, ph * 100 // dpi), Image.LANCZOS)
        prev.save(base + "-preview.png")
        if want_pbm:
            with open(base + ".pbm", "wb") as f:
                f.write(f"P4\n{raster_w} {raster_h}\n".encode())
                f.write(raw.tobytes())
        log(f"wrote {base}.png ({paper}, {dpi} dpi, {page.copies} cop{'y' if page.copies==1 else 'ies'}, "
            f"{'duplex' if page.duplex else 'simplex'}, raster {raster_w}x{raster_h} at +{ox},+{oy})")
    else:
        with open(base + ".pbm", "wb") as f:
            f.write(f"P4\n{raster_w} {raster_h}\n".encode())
            for r in page.rows:
                f.write(r + b"\0" * (stride - len(r)))
        log(f"wrote {base}.pbm (Pillow/numpy not installed; no PNG)")
    return dpi, paper, raster_w, raster_h, clipped


def process_job(data, outdir, want_pbm, verbose, tag=""):
    def log(s):
        if verbose or s.startswith("warning"):
            print(f"{tag}{s}", file=sys.stderr)
    pr = Printer(log)
    try:
        pr.feed(data)
    except StreamError as e:
        pr.warn(f"error: malformed stream: {e}")
    os.makedirs(outdir, exist_ok=True)
    summary = []
    for i, pg in enumerate(pr.pages, 1):
        summary.append(render(pg, i, outdir, want_pbm, log))
    sheets = sum(pg.copies for pg in pr.pages)
    print(f"{tag}job {pr.jobname!r}: {len(pr.pages)} page(s), {sheets} impression(s), "
          f"{len(pr.warnings)} warning(s)", file=sys.stderr)
    return pr, summary


# --------------------------------------------------------------------------
# Network emulation (JetDirect / port 9100)

def listen(port, outdir, want_pbm, verbose):
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            peer = self.client_address[0]
            chunks = []
            self.connection.settimeout(5.0)
            while True:
                try:
                    c = self.connection.recv(65536)
                except OSError:
                    break
                if not c:
                    break
                chunks.append(c)
                # Answer PJL queries interactively like a real printer does.
                joined = b"".join(chunks)
                if b"@PJL INFO" in joined or b"@PJL ECHO" in joined or b"@PJL INQUIRE" in joined:
                    probe = Printer()
                    try:
                        probe.feed(joined)
                    except StreamError:
                        pass
                    for r in probe.pjl_responses:
                        self.connection.sendall(r.encode("latin-1"))
                    probe.pjl_responses.clear()
            data = b"".join(chunks)
            if not data:
                return
            os.makedirs(outdir, exist_ok=True)
            stamp = time.strftime("%Y%m%d-%H%M%S")
            seq = 0
            while os.path.exists(os.path.join(outdir, f"job-{stamp}.prn")):
                seq += 1
                stamp = time.strftime("%Y%m%d-%H%M%S") + f"-{seq}"
            raw = os.path.join(outdir, f"job-{stamp}.prn")
            with open(raw, "wb") as f:
                f.write(data)
            print(f"[{peer}] received {len(data)} bytes -> {raw}", file=sys.stderr)
            process_job(data, os.path.join(outdir, f"job-{stamp}"), want_pbm, verbose, tag=f"[{peer}] ")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", port), Handler) as srv:
        print(f"brsim: emulating {MODEL} on tcp/{port}; spool -> {outdir}", file=sys.stderr)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode", help="render a job file")
    d.add_argument("job")
    d.add_argument("-o", "--out", default="out")
    d.add_argument("--pbm", action="store_true", help="also write raw decoded bitmaps")
    d.add_argument("-v", "--verbose", action="store_true")
    l = sub.add_parser("listen", help="emulate the printer on TCP 9100")
    l.add_argument("--port", type=int, default=9100)
    l.add_argument("-o", "--out", default="spool")
    l.add_argument("--pbm", action="store_true")
    l.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.cmd == "decode":
        with open(a.job, "rb") as f:
            data = f.read()
        pr, _ = process_job(data, a.out, a.pbm, a.verbose)
        sys.exit(1 if any(w.startswith("error") for w in pr.warnings) else 0)
    else:
        listen(a.port, a.out, a.pbm, a.verbose)


if __name__ == "__main__":
    main()
