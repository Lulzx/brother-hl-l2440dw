// brhbp_bench.cc -- encoder throughput and peak working set, in process.
#include "brhbp.h"
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace {
class NullSink final : public brhbp::Sink {
 public:
  size_t bytes = 0;
  bool Write(const uint8_t*, size_t n) override { bytes += n; return true; }
};
long PeakRssKB() {
  struct rusage r{};
  getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
  return r.ru_maxrss / 1024;      // bytes on Darwin
#else
  return r.ru_maxrss;             // KB on Linux
#endif
}
}  // namespace

int main(int argc, char** argv) {
  const int dpi = argc > 1 ? std::atoi(argv[1]) : 600;
  const int iters = argc > 2 ? std::atoi(argv[2]) : 20;
  brhbp::JobSettings js;
  js.paper = brhbp::Paper::kA4;
  js.dpi = dpi;

  const brhbp::PageGeometry g = brhbp::ComputeGeometry(js.paper, dpi);
  const long baseline = PeakRssKB();

  // Synthetic text-like page: mostly white, periodic ink, high vertical
  // coherence -- the case the delta encoder is built for.
  std::vector<uint8_t> page(g.stride * static_cast<size_t>(g.rows), 0);
  for (int y = 0; y < g.rows; ++y)
    if ((y / 40) % 3 == 0)
      for (size_t x = 0; x < g.stride; x += 5)
        page[static_cast<size_t>(y) * g.stride + x] = 0x3C;
  const long after_page = PeakRssKB();

  size_t out = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    NullSink sink;
    brhbp::JobEncoder enc(&sink, js);
    enc.Begin();
    enc.BeginPage();
    for (int y = 0; y < g.rows; ++y)
      enc.WriteRow(page.data() + static_cast<size_t>(y) * g.stride);
    enc.EndPage();
    enc.End();
    out = sink.bytes;
  }
  auto t1 = std::chrono::steady_clock::now();
  const long after_enc = PeakRssKB();

  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
  const double in_mb = static_cast<double>(page.size()) / 1e6;
  std::printf("A4 @%d dpi  %dx%d, stride %zu\n", dpi, g.width_px, g.rows, g.stride);
  std::printf("  input      %.1f MB (1-bit page)\n", in_mb);
  std::printf("  output     %.2f MB\n", out / 1e6);
  std::printf("  encode     %.2f ms/page  =  %.0f MB/s in, %.0f pages/s\n",
              ms, in_mb / (ms / 1000.0), 1000.0 / ms);
  std::printf("  peak RSS   baseline %ld KB -> +page %ld KB -> +encoder %ld KB"
              "  (encoder cost %ld KB)\n",
              baseline, after_page, after_enc, after_enc - after_page);
  return 0;
}
