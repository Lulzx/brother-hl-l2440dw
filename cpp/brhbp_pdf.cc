// brhbp_pdf.cc -- PDF in, HBP job out, in constant memory.
//
// This is the piece that makes the encoder a print path rather than a library.
// MuPDF renders one 64-row band at a time straight into a band-sized pixmap;
// the band is halftoned to 1 bit, fed to the encoder, and dropped. No page
// bitmap is ever allocated, at any resolution.
//
//   working set = one gray band + one 1-bit row + the encoder's 64 KB
//               ~ 3 MB at A4/600, ~6 MB at A4/1200
//   versus       139 MB for a single ARGB_8888 A4 page at 600 dpi
//
// Halftoning is an ordered (Bayer) dither on purpose. It is stateless per
// pixel, so a band's output does not depend on the band above it -- which is
// what keeps bands independent and the pipeline streamable. Error diffusion
// would give better tone but carries state across the boundary.

#include "brhbp.h"

#include <mupdf/fitz.h>

#include <sys/resource.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

namespace {

class StdoutSink final : public brhbp::Sink {
 public:
  bool Write(const uint8_t* d, size_t n) override {
    return std::fwrite(d, 1, n, stdout) == n;
  }
};

class SocketlessNull final : public brhbp::Sink {
 public:
  size_t bytes = 0;
  bool Write(const uint8_t*, size_t n) override { bytes += n; return true; }
};

// 8x8 Bayer matrix, values 0..63.
const uint8_t kBayer[64] = {
   0,32, 8,40, 2,34,10,42,  48,16,56,24,50,18,58,26,
  12,44, 4,36,14,46, 6,38,  60,28,52,20,62,30,54,22,
   3,35,11,43, 1,33, 9,41,  51,19,59,27,49,17,57,25,
  15,47, 7,39,13,45, 5,37,  63,31,55,23,61,29,53,21,
};

long PeakRssKB() {
  struct rusage r{};
  getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
  return r.ru_maxrss / 1024;
#else
  return r.ru_maxrss;
#endif
}

brhbp::Paper ParsePaper(const char* s) {
  struct { const char* n; brhbp::Paper p; } m[] = {
    {"A4", brhbp::Paper::kA4}, {"LETTER", brhbp::Paper::kLetter},
    {"LEGAL", brhbp::Paper::kLegal}, {"A5", brhbp::Paper::kA5},
    {"A6", brhbp::Paper::kA6}, {"B5", brhbp::Paper::kB5},
    {"B6", brhbp::Paper::kB6}, {"EXECUTIVE", brhbp::Paper::kExecutive},
    {"C5", brhbp::Paper::kC5}, {"DL", brhbp::Paper::kDL},
    {"MONARCH", brhbp::Paper::kMonarch},
  };
  for (auto& e : m) if (!strcasecmp(e.n, s)) return e.p;
  std::fprintf(stderr, "unknown paper %s\n", s); std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  brhbp::JobSettings js;
  const char* path = nullptr;
  bool threshold_only = false, quiet = false, discard = false, diffuse = false;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!std::strcmp(a, "-p") && i + 1 < argc) js.paper = ParsePaper(argv[++i]);
    else if (!std::strcmp(a, "-r") && i + 1 < argc) js.dpi = std::atoi(argv[++i]);
    else if (!std::strcmp(a, "-c") && i + 1 < argc) js.copies = std::atoi(argv[++i]);
    else if (!std::strcmp(a, "-j") && i + 1 < argc) js.job_name = argv[++i];
    else if (!std::strcmp(a, "-d")) js.duplex = brhbp::Duplex::kLongEdge;
    else if (!std::strcmp(a, "-e")) js.toner_save = true;
    else if (!std::strcmp(a, "-t")) threshold_only = true;
    else if (!std::strcmp(a, "-E")) diffuse = true;
    else if (!std::strcmp(a, "-q")) quiet = true;
    else if (!std::strcmp(a, "-n")) discard = true;   // benchmark: encode, drop
    else if (a[0] != '-') path = a;
    else { std::fprintf(stderr, "unknown option %s\n", a); return 2; }
  }
  if (!path) {
    std::fprintf(stderr,
      "usage: brhbp_pdf [-p PAPER] [-r DPI] [-c N] [-d] [-e] [-t] [-n] doc.pdf > job.prn\n"
      "  -t  plain threshold instead of ordered dither\n"
      "  -E  Floyd-Steinberg error diffusion (better photos, see note)\n"
      "  -n  encode but discard output (benchmark)\n");
    return 2;
  }

  fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
  if (!ctx) { std::fprintf(stderr, "cannot create mupdf context\n"); return 1; }
  fz_register_document_handlers(ctx);

  StdoutSink out_sink;
  SocketlessNull null_sink;
  brhbp::Sink* sink = discard ? static_cast<brhbp::Sink*>(&null_sink)
                              : static_cast<brhbp::Sink*>(&out_sink);
  brhbp::JobEncoder enc(sink, js);
  const brhbp::PageGeometry g = enc.geometry();

  const int kBand = 64;
  std::vector<uint8_t> row(g.stride);
  // Error diffusion carries residue into the row below, so it must cross band
  // boundaries. Keeping one row of error here -- rather than inside the band
  // loop -- is what lets it stream: bands still go out one at a time, they
  // just have to go out *in order*. That rules out encoding bands in parallel,
  // which ordered dither would allow. Deliberate trade, hence opt-in.
  std::vector<int32_t> err_cur, err_next;
  if (diffuse) { err_cur.assign(g.width_px + 2, 0); err_next.assign(g.width_px + 2, 0); }
  const long rss_start = PeakRssKB();

  fz_document* doc = nullptr;
  fz_try(ctx) doc = fz_open_document(ctx, path);
  fz_catch(ctx) {
    std::fprintf(stderr, "cannot open %s: %s\n", path, fz_caught_message(ctx));
    fz_drop_context(ctx); return 1;
  }

  const int npages = fz_count_pages(ctx, doc);
  if (!enc.Begin()) { std::fprintf(stderr, "sink error\n"); return 1; }

  const float scale = js.dpi / 72.0f;
  for (int pno = 0; pno < npages; ++pno) {
    fz_page* page = fz_load_page(ctx, doc, pno);
    const fz_rect bounds = fz_bound_page(ctx, page);
    // Device space: printable-area top-left becomes pixel (0,0).
    fz_matrix m = fz_scale(scale, scale);
    m = fz_concat(m, fz_translate(-bounds.x0 * scale - g.origin_x,
                                  -bounds.y0 * scale - g.origin_y));

    if (!enc.BeginPage()) { std::fprintf(stderr, "sink error\n"); return 1; }
    if (diffuse) { std::fill(err_cur.begin(), err_cur.end(), 0);
                   std::fill(err_next.begin(), err_next.end(), 0); }

    for (int y0 = 0; y0 < g.rows; y0 += kBand) {
      const int y1 = (y0 + kBand < g.rows) ? y0 + kBand : g.rows;
      fz_irect bbox = { 0, y0, g.width_px, y1 };
      fz_pixmap* pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), bbox, nullptr, 0);
      fz_clear_pixmap_with_value(ctx, pix, 0xff);            // white
      fz_device* dev = fz_new_draw_device(ctx, fz_identity, pix);
      fz_try(ctx) fz_run_page(ctx, page, dev, m, nullptr);
      fz_always(ctx) { fz_close_device(ctx, dev); fz_drop_device(ctx, dev); }
      fz_catch(ctx) { fz_drop_pixmap(ctx, pix); fz_drop_page(ctx, page);
                      std::fprintf(stderr, "render failed on page %d\n", pno + 1); return 1; }

      const uint8_t* s = fz_pixmap_samples(ctx, pix);
      const int st = fz_pixmap_stride(ctx, pix);
      for (int y = y0; y < y1; ++y) {
        std::memset(row.data(), 0, g.stride);
        const uint8_t* src = s + static_cast<size_t>(y - y0) * st;
        if (diffuse) {
          err_cur.swap(err_next);
          std::fill(err_next.begin(), err_next.end(), 0);
          for (int x = 0; x < g.width_px; ++x) {
            const int old = src[x] + err_cur[x + 1];
            const int nv  = old < 128 ? 0 : 255;
            if (nv == 0) row[x >> 3] |= 0x80 >> (x & 7);
            const int e = old - nv;
            err_cur [x + 2] += e * 7 / 16;
            err_next[x    ] += e * 3 / 16;
            err_next[x + 1] += e * 5 / 16;
            err_next[x + 2] += e * 1 / 16;
          }
        } else if (threshold_only) {
          for (int x = 0; x < g.width_px; ++x)
            if (src[x] < 128) row[x >> 3] |= 0x80 >> (x & 7);
        } else {
          const uint8_t* brow = kBayer + ((y & 7) << 3);
          for (int x = 0; x < g.width_px; ++x) {
            const int t = (brow[x & 7] * 255 + 32) / 64;
            if (src[x] <= t) row[x >> 3] |= 0x80 >> (x & 7);
          }
        }
        if (!enc.WriteRow(row.data())) { std::fprintf(stderr, "sink error\n"); return 1; }
      }
      fz_drop_pixmap(ctx, pix);           // band is gone before the next one
    }
    if (!enc.EndPage()) { std::fprintf(stderr, "sink error\n"); return 1; }
    fz_drop_page(ctx, page);
  }
  if (!enc.End()) { std::fprintf(stderr, "sink error\n"); return 1; }

  fz_drop_document(ctx, doc);
  fz_drop_context(ctx);

  if (!quiet) {
    std::fprintf(stderr,
        "brhbp_pdf: %d page(s) at %d dpi, %dx%d, band %d rows\n"
        "           peak RSS %ld KB (start %ld KB)%s\n",
        npages, js.dpi, g.width_px, g.rows, kBand,
        PeakRssKB(), rss_start,
        discard ? "" : "");
    if (discard) std::fprintf(stderr, "           output %zu bytes (discarded)\n", null_sink.bytes);
  }
  return 0;
}
