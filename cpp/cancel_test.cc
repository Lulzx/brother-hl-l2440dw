// cancel_test.cc -- produce three streams for the simulator to judge:
//
//   clean.prn      3 pages, no cancellation          (control)
//   aborted.prn    cancelled mid page 2, Abort() called
//   truncated.prn  cancelled mid page 2, socket just stops (no Abort)
//
// The question each answers: after a cancel, how many pages does the device
// commit, and is it left in a sane state? The third case is what a naive
// implementation does and is included to show the difference.
#include "brhbp.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
class FileSink final : public brhbp::Sink {
 public:
  explicit FileSink(const char* p) { f_ = std::fopen(p, "wb"); }
  ~FileSink() { if (f_) std::fclose(f_); }
  bool Write(const uint8_t* d, size_t n) override {
    return f_ && std::fwrite(d, 1, n, f_) == n;
  }
 private:
  FILE* f_ = nullptr;
};

void FillRow(std::vector<uint8_t>& row, int y) {
  std::memset(row.data(), 0, row.size());
  if ((y / 30) % 2 == 0)
    for (size_t x = 0; x < row.size(); x += 4) row[x] = 0x7E;
}
}  // namespace

int main() {
  brhbp::JobSettings js;
  js.paper = brhbp::Paper::kA4;
  js.dpi = 600;
  js.job_name = "canceltest";
  const brhbp::PageGeometry g = brhbp::ComputeGeometry(js.paper, js.dpi);
  std::vector<uint8_t> row(g.stride);

  // 1. Control: three complete pages.
  {
    FileSink s("/tmp/clean.prn");
    brhbp::JobEncoder e(&s, js);
    e.Begin();
    for (int p = 0; p < 3; ++p) {
      e.BeginPage();
      for (int y = 0; y < g.rows; ++y) { FillRow(row, y); e.WriteRow(row.data()); }
      e.EndPage();
    }
    e.End();
  }

  // 2 and 3: cancel a third of the way into page 2.
  const int cancel_row = g.rows / 3;
  for (int variant = 0; variant < 2; ++variant) {
    const bool call_abort = (variant == 0);
    FileSink s(call_abort ? "/tmp/aborted.prn" : "/tmp/truncated.prn");
    brhbp::JobEncoder e(&s, js);
    e.Begin();
    bool stop = false;
    for (int p = 0; p < 3 && !stop; ++p) {
      if (!e.BeginPage()) { stop = true; break; }
      for (int y = 0; y < g.rows; ++y) {
        if (p == 1 && y == cancel_row) e.RequestCancel();   // async signal
        FillRow(row, y);
        if (!e.WriteRow(row.data())) { stop = true; break; }
      }
      if (!stop) e.EndPage();
    }
    if (e.status() != brhbp::Status::kCancelled) {
      std::fprintf(stderr, "FAIL: expected kCancelled, got %d\n",
                   static_cast<int>(e.status()));
      return 1;
    }
    if (call_abort && !e.Abort()) { std::fprintf(stderr, "FAIL: Abort()\n"); return 1; }
    // variant 1 deliberately emits nothing further: the socket just stops.
  }
  std::printf("cancel row = %d of %d on page 2\n", cancel_row, g.rows);
  return 0;
}
