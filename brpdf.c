/*
 * brpdf.c -- tiny driver-free job encoder for Brother HL-L2400-series
 *            host-based ("GDI"/HBP) mono laser printers, e.g. HL-L2440DW.
 *
 * Reads a stream of 1-bit PBM (P4) pages on stdin (one page per image,
 * concatenated), and writes a complete print job on stdout that can be
 * sent verbatim to the printer (TCP 9100, USB, or IPP raw).
 *
 *   mutool draw -F pbm -r 600 -o - doc.pdf | ./brpdf -p LETTER > job.prn
 *   nc printer 9100 < job.prn
 *
 * Wire format (see FORMAT.md): PJL job envelope, a PCL-style page header,
 * then raster bands in Brother's "mode 1030" line-delta compression.
 * The band/line encoder here is a byte-for-byte port of the algorithm in
 * brlaser (GPL-2, Peter De Wachter), which is the proven reference for this
 * printer family.  This file is dependency free (C99 + libc only).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */

typedef struct { uint8_t *d; size_t n, cap; } buf;

static void buf_put(buf *b, uint8_t c) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->d = realloc(b->d, b->cap);
        if (!b->d) { perror("realloc"); exit(1); }
    }
    b->d[b->n++] = c;
}
static void buf_append(buf *b, const uint8_t *p, size_t len) {
    while (len--) buf_put(b, *p++);
}

/* ------------------------------------------------------------------ */
/* Mode-1030 line encoder (port of brlaser line.cc)                    */

static int imin(int a, int b) { return a < b ? a : b; }

static void write_overflow(buf *o, int v) {
    if (v < 0) return;
    if (v < 255) { buf_put(o, (uint8_t)v); return; }
    for (int i = 0; i < v / 255; i++) buf_put(o, 255);
    buf_put(o, (uint8_t)(v % 255));
}

/* Substitute: skip `offset` bytes of the reference, then emit `len` literals. */
static void write_substitute(buf *o, int offset, const uint8_t *p, int len) {
    int count = len - 1;
    int offset_low = imin(offset, 15), count_low = imin(count, 7);
    buf_put(o, (uint8_t)((offset_low << 3) | count_low));
    write_overflow(o, offset - 15);
    write_overflow(o, count - 7);
    buf_append(o, p, (size_t)len);
}

/* Repeat: skip `offset` bytes, then emit `count` copies of `value`. */
static void write_repeat(buf *o, int offset, int count, uint8_t value) {
    count -= 2;
    int offset_low = imin(offset, 3), count_low = imin(count, 31);
    buf_put(o, (uint8_t)(128 | (offset_low << 5) | count_low));
    write_overflow(o, offset - 3);
    write_overflow(o, count - 31);
    buf_put(o, value);
}

static int all_zeros(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static int repeat_length(const uint8_t *p, int n) {
    if (n <= 0) return 0;
    int i = 1;
    while (i < n && p[i] == p[0]) i++;
    return i;
}

/* How many literal bytes to emit before either the line re-synchronises
 * with the reference (two equal bytes in a row) or a run starts. */
static int substitute_length(const uint8_t *l, const uint8_t *r, int n) {
    if (n <= 0) return 0;
    int it = 0, next = 1, prev = 0;
    while (next < n) {
        if (l[it] == r[it] && l[next] == r[next]) return it;
        if (l[it] == l[next] && l[it] == l[prev]) return prev;
        prev = it; it = next; next++;
    }
    return n;
}

/* Encode `line` as edits relative to `ref` (both `n` bytes). */
static void encode_line_ref(buf *o, const uint8_t *line, const uint8_t *ref, int n) {
    o->n = 0;
    if (all_zeros(line, n)) { buf_put(o, 0xFF); return; }
    buf_put(o, 0);                       /* edit count, patched below */
    int end = n;
    while (end > 0 && line[end - 1] == ref[end - 1]) end--;
    int i = 0, edits = 0;
    for (;;) {
        int start = i;
        while (i < end && line[i] == ref[i]) i++;
        int offset = i - start;
        if (i == end) break;
        if (++edits == 254) {            /* out of edits: dump the rest */
            write_substitute(o, offset, line + i, end - i);
            break;
        }
        int s = substitute_length(line + i, ref + i, end - i);
        if (s > 0) {
            write_substitute(o, offset, line + i, s);
            i += s;
        } else {
            int r = repeat_length(line + i, end - i);
            write_repeat(o, offset, r, line[i]);
            i += r;
        }
    }
    o->d[0] = (uint8_t)edits;
}

/* Encode `line` with no reference (first line of a band). */
static void encode_line_noref(buf *o, const uint8_t *line, int n) {
    o->n = 0;
    if (all_zeros(line, n)) { buf_put(o, 0xFF); return; }
    buf_put(o, 1);
    write_substitute(o, 0, line, n);
}

/* ------------------------------------------------------------------ */
/* Band ("block") writer                                               */

#define MAX_BLOCK 16350
#define LINES_PER_BAND 64       /* brlaser uses 64; Brother's driver 128 */

static buf blk;                 /* concatenated encoded lines */
static int blk_lines;

static int block_fits(size_t sz) { return blk.n + sz < MAX_BLOCK; }

static void block_flush(FILE *f) {
    if (!blk_lines) return;
    fprintf(f, "%zuw", blk.n + 2);
    putc(0, f);
    putc(blk_lines, f);
    fwrite(blk.d, 1, blk.n, f);
    blk.n = 0; blk_lines = 0;
}

static void block_add(const buf *l) {
    buf_append(&blk, l->d, l->n);
    blk_lines++;
}

/* ------------------------------------------------------------------ */
/* Page geometry                                                       */

struct paper { const char *name; int w_pt, h_pt; };
static const struct paper papers[] = {
    {"A4", 595, 842},   {"LETTER", 612, 792}, {"LEGAL", 612, 1008},
    {"A5", 420, 595},   {"A6", 298, 420},     {"B5", 516, 729},
    {"B6", 363, 516},   {"EXECUTIVE", 522, 756}, {"FOLIO", 612, 936},
    {"C5", 459, 649},   {"DL", 312, 624},     {"MONARCH", 279, 540},
};
/* Unprintable borders in points (from brlaser's PPD: HWMargins 8 8 8 16 =
 * left bottom right top). The engine places the raster at the top-left of
 * the printable area; there is no positioning command in the stream. */
#define MARGIN_L 8
#define MARGIN_R 8
#define MARGIN_T 16
#define MARGIN_B 8

static const struct paper *find_paper(const char *s) {
    for (size_t i = 0; i < sizeof papers / sizeof *papers; i++)
        if (!strcasecmp(papers[i].name, s)) return &papers[i];
    return NULL;
}
static int pt2px(int pt, int dpi) { return pt * dpi / 72; }

/* ------------------------------------------------------------------ */
/* PBM (P4) reader                                                     */

static int pbm_int(FILE *f) {
    int c, v = 0;
    do {
        c = getc(f);
        if (c == '#') { while (c != '\n' && c != EOF) c = getc(f); }
    } while (isspace(c) || c == '#');
    if (!isdigit(c)) return -1;
    while (isdigit(c)) { v = v * 10 + (c - '0'); c = getc(f); }
    return v;                    /* consumed exactly one trailing ws byte */
}

/* Returns 1 on success and fills w,h,bits (row-major, ceil(w/8) bytes/row). */
static int pbm_read(FILE *f, int *w, int *h, uint8_t **bits) {
    int c;
    while ((c = getc(f)) != EOF && c != 'P') {}
    if (c == EOF) return 0;
    if (getc(f) != '4') { fprintf(stderr, "brpdf: input is not raw PBM (P4)\n"); exit(1); }
    *w = pbm_int(f); *h = pbm_int(f);
    if (*w <= 0 || *h <= 0) { fprintf(stderr, "brpdf: bad PBM header\n"); exit(1); }
    size_t stride = ((size_t)*w + 7) / 8, sz = stride * (size_t)*h;
    *bits = malloc(sz);
    if (!*bits || fread(*bits, 1, sz, f) != sz) { fprintf(stderr, "brpdf: truncated PBM\n"); exit(1); }
    return 1;
}

/* Rotate a 1-bpp bitmap 180 degrees in place (pixel exact for any width). */
static void rotate180(uint8_t *bits, int w, int h) {
    size_t stride = ((size_t)w + 7) / 8;
    uint8_t *out = calloc(stride, (size_t)h);
    for (int y = 0; y < h; y++) {
        const uint8_t *src = bits + (size_t)y * stride;
        uint8_t *dst = out + (size_t)(h - 1 - y) * stride;
        for (int x = 0; x < w; x++)
            if (src[x >> 3] & (0x80 >> (x & 7))) {
                int nx = w - 1 - x;
                dst[nx >> 3] |= 0x80 >> (nx & 7);
            }
    }
    memcpy(bits, out, stride * (size_t)h);
    free(out);
}

/* ------------------------------------------------------------------ */
/* Job / page output                                                   */

struct opts {
    const struct paper *paper;
    int dpi, copies, duplex, economode, raw, norotate;
    const char *tray, *media, *job;
};

static void write_job_header(FILE *f, const struct opts *o) {
    for (int i = 0; i < 128; i++) putc(0, f);     /* wake-up padding */
    fprintf(f, "\033%%-12345X@PJL\n");
    fprintf(f, "@PJL JOB NAME=\"%s\"\n", o->job);
}

static void write_page_header(FILE *f, const struct opts *o) {
    fprintf(f, "\033%%-12345X@PJL\n");
    if (o->dpi == 1200) {
        fprintf(f, "@PJL SET RAS1200MODE = TRUE\n");
        fprintf(f, "@PJL SET RESOLUTION = 600\n");
    } else {
        fprintf(f, "@PJL SET RAS1200MODE = FALSE\n");
        fprintf(f, "@PJL SET RESOLUTION = %d\n", o->dpi);
    }
    fprintf(f, "@PJL SET ECONOMODE = %s\n", o->economode ? "ON" : "OFF");
    fprintf(f, "@PJL SET SOURCETRAY = %s\n", o->tray);
    fprintf(f, "@PJL SET MEDIATYPE = %s\n", o->media);
    fprintf(f, "@PJL SET PAPER = %s\n", o->paper->name);
    fprintf(f, "@PJL SET PAGEPROTECT = AUTO\n");
    fprintf(f, "@PJL SET ORIENTATION = PORTRAIT\n");
    fprintf(f, "@PJL ENTER LANGUAGE = PCL\n");
    fputs("\033E", f);                             /* PCL reset */
    fprintf(f, "\033&l%dX", o->copies);            /* copies */
    if (o->duplex) fputs("\033&l2S", f);           /* long-edge duplex */
}

static void write_job_trailer(FILE *f, const struct opts *o) {
    fprintf(f, "\033%%-12345X@PJL\n");
    fprintf(f, "@PJL EOJ NAME=\"%s\"\n", o->job);
    fprintf(f, "\033%%-12345X\n");
}

/* Emit one page: `lines` rows of `stride` bytes, fetched via getrow(). */
static void encode_page(FILE *f, int lines, int stride,
                        const uint8_t *(*getrow)(int y, void *ctx), void *ctx) {
    static buf enc; static uint8_t *ref;
    ref = realloc(ref, (size_t)stride);
    memset(ref, 0, (size_t)stride);

    const uint8_t *row = getrow(0, ctx);
    encode_line_noref(&enc, row, stride);
    block_add(&enc);
    memcpy(ref, row, (size_t)stride);

    fputs("\033*b1030m", f);                       /* Brother compression */
    for (int y = 1; y < lines; y++) {
        row = getrow(y, ctx);
        if (y % LINES_PER_BAND == 0) {
            block_flush(f);
            encode_line_noref(&enc, row, stride);
        } else {
            encode_line_ref(&enc, row, ref, stride);
            if (!block_fits(enc.n)) {
                block_flush(f);
                encode_line_noref(&enc, row, stride);
            }
        }
        block_add(&enc);
        memcpy(ref, row, (size_t)stride);
    }
    block_flush(f);
    fputs("1030M\f", f);                           /* end raster, eject */
}

/* Row provider that crops/pads the source bitmap into the printable area. */
struct src {
    const uint8_t *bits; int w, h; size_t stride;
    int x0, y0;              /* source offset of printable origin (may be <0 == pad) */
    int out_stride;
    uint8_t *tmp;
};
static const uint8_t *getrow_crop(int y, void *ctx) {
    struct src *s = ctx;
    memset(s->tmp, 0, (size_t)s->out_stride);
    int sy = y + s->y0;
    if (sy < 0 || sy >= s->h) return s->tmp;
    const uint8_t *r = s->bits + (size_t)sy * s->stride;
    int x0 = s->x0;
    if (x0 % 8 == 0) {                     /* fast byte-aligned path */
        int b0 = x0 / 8;
        for (int i = 0; i < s->out_stride; i++) {
            int sb = b0 + i;
            if (sb >= 0 && (size_t)sb < s->stride) s->tmp[i] = r[sb];
        }
    } else {
        for (int x = 0; x < s->out_stride * 8; x++) {
            int sx = x + x0;
            if (sx >= 0 && sx < s->w && (r[sx >> 3] & (0x80 >> (sx & 7))))
                s->tmp[x >> 3] |= 0x80 >> (x & 7);
        }
    }
    return s->tmp;
}

static void usage(void) {
    fputs(
"usage: brpdf [options] < pages.pbm > job.prn\n"
"  -p PAPER   A4 (default) | LETTER | LEGAL | A5 | A6 | B5 | B6 | EXECUTIVE | FOLIO | C5 | DL | MONARCH\n"
"  -r DPI     300 | 600 (default) | 1200   (must match the PBM rendering resolution)\n"
"  -c N       copies (default 1)\n"
"  -d         duplex (long edge); back pages are rotated 180 unless -R\n"
"  -R         do not rotate back pages in duplex mode\n"
"  -e         toner-save (ECONOMODE=ON)\n"
"  -t TRAY    AUTO (default) | T1 | MP | MANUAL\n"
"  -m MEDIA   PLAIN (default) | THIN | THICK | THICKER | BOND | ENV | ...\n"
"  -j NAME    PJL job name (default brpdf)\n"
"  -x         raw: send input bitmap as-is (no margin crop/pad), for testing\n"
"Input: concatenated raw PBM pages, e.g.  mutool draw -F pbm -r 600 -o - doc.pdf\n"
"If a page is rendered at full physical paper size it is cropped to the\n"
"printable area (8pt sides/bottom, 16pt top); smaller bitmaps are placed at\n"
"the printable origin.\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    struct opts o = { find_paper("A4"), 600, 1, 0, 0, 0, 0, "AUTO", "PLAIN", "brpdf" };
    int c;
    while ((c = getopt(argc, argv, "p:r:c:dRet:m:j:xh")) != -1) {
        switch (c) {
        case 'p': o.paper = find_paper(optarg); if (!o.paper) { fprintf(stderr, "brpdf: unknown paper %s\n", optarg); usage(); } break;
        case 'r': o.dpi = atoi(optarg); if (o.dpi != 300 && o.dpi != 600 && o.dpi != 1200) usage(); break;
        case 'c': o.copies = atoi(optarg); if (o.copies < 1) o.copies = 1; break;
        case 'd': o.duplex = 1; break;
        case 'R': o.norotate = 1; break;
        case 'e': o.economode = 1; break;
        case 't': o.tray = optarg; break;
        case 'm': o.media = optarg; break;
        case 'j': o.job = optarg; break;
        case 'x': o.raw = 1; break;
        default: usage();
        }
    }

    int phys_w = pt2px(o.paper->w_pt, o.dpi), phys_h = pt2px(o.paper->h_pt, o.dpi);
    int print_w = pt2px(o.paper->w_pt - MARGIN_L - MARGIN_R, o.dpi);
    int print_h = pt2px(o.paper->h_pt - MARGIN_T - MARGIN_B, o.dpi);
    int ml = pt2px(MARGIN_L, o.dpi), mt = pt2px(MARGIN_T, o.dpi);

    int page = 0, w, h; uint8_t *bits;
    while (pbm_read(stdin, &w, &h, &bits)) {
        if (page == 0) { write_job_header(stdout, &o); write_page_header(stdout, &o); }
        page++;

        int back = o.duplex && !o.norotate && (page % 2 == 0);
        if (back) rotate180(bits, w, h);

        struct src s = { bits, w, h, ((size_t)w + 7) / 8, 0, 0, 0, NULL };
        int lines;
        if (o.raw) {
            s.out_stride = (int)s.stride; lines = h;
        } else {
            /* Full-size page render? Then the printable window starts at the
             * physical margins; otherwise place the bitmap at the origin. */
            int full = w >= phys_w - ml && h >= phys_h - mt;
            s.x0 = full ? ml : 0;
            s.y0 = full ? mt : 0;
            s.out_stride = (print_w + 7) / 8;
            lines = print_h;
            if (w > print_w * 3 / 2 && !full)
                fprintf(stderr, "brpdf: page %d: bitmap %dx%d does not match %s at %d dpi (%dx%d); it will be clipped\n",
                        page, w, h, o.paper->name, o.dpi, phys_w, phys_h);
        }
        s.tmp = calloc(1, (size_t)s.out_stride);
        encode_page(stdout, lines, s.out_stride, getrow_crop, &s);
        fprintf(stderr, "brpdf: page %d  %dx%d -> %d lines x %d bytes%s\n",
                page, w, h, lines, s.out_stride, back ? " (back, rotated)" : "");
        free(s.tmp); free(bits);
    }
    if (page == 0) { fprintf(stderr, "brpdf: no pages on stdin\n"); return 1; }
    write_job_trailer(stdout, &o);
    fflush(stdout);
    return ferror(stdout) ? 1 : 0;
}
