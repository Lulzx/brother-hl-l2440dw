// brhbp_tool.cc -- PBM on stdin, HBP job on stdout. Mirrors brpdf's CLI so the
// two can be diffed byte for byte; the point of this tool is the conformance
// check, not the interface. Real callers use JobEncoder directly and never
// materialise a page.
#include "brhbp.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

class StdoutSink final : public brhbp::Sink {
 public:
  bool Write(const uint8_t* d, size_t n) override {
    return std::fwrite(d, 1, n, stdout) == n;
  }
};

int PbmInt(FILE* f) {
  int c, v = 0;
  do {
    c = std::getc(f);
    if (c == '#') while (c != '\n' && c != EOF) c = std::getc(f);
  } while (std::isspace(c) || c == '#');
  if (!std::isdigit(c)) return -1;
  while (std::isdigit(c)) { v = v * 10 + (c - '0'); c = std::getc(f); }
  return v;
}

bool PbmRead(FILE* f, int* w, int* h, std::vector<uint8_t>* bits) {
  int c;
  while ((c = std::getc(f)) != EOF && c != 'P') {}
  if (c == EOF) return false;
  if (std::getc(f) != '4') { std::fprintf(stderr, "not raw PBM (P4)\n"); std::exit(1); }
  *w = PbmInt(f); *h = PbmInt(f);
  if (*w <= 0 || *h <= 0) { std::fprintf(stderr, "bad PBM header\n"); std::exit(1); }
  size_t stride = (static_cast<size_t>(*w) + 7) / 8;
  bits->resize(stride * static_cast<size_t>(*h));
  if (std::fread(bits->data(), 1, bits->size(), f) != bits->size()) {
    std::fprintf(stderr, "truncated PBM\n"); std::exit(1);
  }
  return true;
}

void Rotate180(std::vector<uint8_t>& bits, int w, int h) {
  size_t stride = (static_cast<size_t>(w) + 7) / 8;
  std::vector<uint8_t> out(stride * static_cast<size_t>(h), 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* src = bits.data() + static_cast<size_t>(y) * stride;
    uint8_t* dst = out.data() + static_cast<size_t>(h - 1 - y) * stride;
    for (int x = 0; x < w; ++x)
      if (src[x >> 3] & (0x80 >> (x & 7))) {
        int nx = w - 1 - x;
        dst[nx >> 3] |= 0x80 >> (nx & 7);
      }
  }
  bits.swap(out);
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
  bool norotate = false;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!std::strcmp(a, "-p") && i + 1 < argc) js.paper = ParsePaper(argv[++i]);
    else if (!std::strcmp(a, "-r") && i + 1 < argc) js.dpi = std::atoi(argv[++i]);
    else if (!std::strcmp(a, "-c") && i + 1 < argc) js.copies = std::atoi(argv[++i]);
    else if (!std::strcmp(a, "-j") && i + 1 < argc) js.job_name = argv[++i];
    else if (!std::strcmp(a, "-d")) js.duplex = brhbp::Duplex::kLongEdge;
    else if (!std::strcmp(a, "-e")) js.toner_save = true;
    else if (!std::strcmp(a, "-R")) norotate = true;
    else { std::fprintf(stderr, "unknown option %s\n", a); return 2; }
  }

  StdoutSink sink;
  brhbp::JobEncoder enc(&sink, js);
  const brhbp::PageGeometry& g = enc.geometry();

  std::vector<uint8_t> bits, rowbuf(g.stride);
  int w, h, page = 0;
  bool began = false;
  while (PbmRead(stdin, &w, &h, &bits)) {
    if (!began) { if (!enc.Begin()) return 1; began = true; }
    ++page;
    const bool back = js.duplex == brhbp::Duplex::kLongEdge && !norotate && (page % 2 == 0);
    if (back) Rotate180(bits, w, h);

    const size_t src_stride = (static_cast<size_t>(w) + 7) / 8;
    // brpdf places the printable window at the physical margins when the
    // source is a full-size page render, and at the origin otherwise.
    const bool full = (w >= g.width_px) && (h >= g.rows);
    const int x0 = full ? g.origin_x : 0;
    const int y0 = full ? g.origin_y : 0;

    if (!enc.BeginPage()) return 1;
    for (int y = 0; y < g.rows; ++y) {
      std::memset(rowbuf.data(), 0, g.stride);
      const int sy = y + y0;
      if (sy >= 0 && sy < h) {
        const uint8_t* r = bits.data() + static_cast<size_t>(sy) * src_stride;
        if (x0 % 8 == 0) {
          const int b0 = x0 / 8;
          for (size_t i = 0; i < g.stride; ++i) {
            const long sb = b0 + static_cast<long>(i);
            if (sb >= 0 && static_cast<size_t>(sb) < src_stride) rowbuf[i] = r[sb];
          }
        } else {
          for (size_t x = 0; x < g.stride * 8; ++x) {
            const long sx = static_cast<long>(x) + x0;
            if (sx >= 0 && sx < w && (r[sx >> 3] & (0x80 >> (sx & 7))))
              rowbuf[x >> 3] |= 0x80 >> (x & 7);
          }
        }
      }
      if (!enc.WriteRow(rowbuf.data())) return 1;
    }
    if (!enc.EndPage()) return 1;
    std::fprintf(stderr, "brhbp: page %d  %dx%d -> %d lines x %zu bytes%s\n",
                 page, w, h, g.rows, g.stride, back ? " (back, rotated)" : "");
  }
  if (began && !enc.End()) return 1;
  return 0;
}
