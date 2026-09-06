// example_stream.cc -- the shape that makes an app feel fast.
//
// The point is what is NOT here: no page bitmap, no temp files, no second
// pass over the document.  Page 1 reaches the printer as soon as page 1 is
// rendered, and pages 2..N are rendered inside the ~14 s the engine spends
// on each preceding sheet.  Time to first sheet stops depending on page count.

#include "brhbp.h"

#include <sys/socket.h>

#include <cstdio>
#include <vector>

namespace {

// Your rasteriser, whatever it is: MuPDF, PdfRenderer via JNI, CoreGraphics.
// The only contract is "fill `dst` with `count` rows of 1-bit, MSB-first,
// 1 = black, `stride` bytes each, starting at row `first_row` of page
// `page`".  Rendering a band rather than a page is what keeps memory flat.
bool RenderBand(int page, int first_row, int count,
                size_t stride, uint8_t* dst);

int PageCount();

// A Sink that writes to an already-connected socket.  Note it is created
// once for the whole document: port 9100 accepts one connection at a time
// and refuses the next for a second or two after close, so opening a socket
// per page is a guaranteed stall.
class SocketSink final : public brhbp::Sink {
 public:
  explicit SocketSink(int fd) : fd_(fd) {}
  bool Write(const uint8_t* data, size_t len) override {
    while (len > 0) {
      ssize_t n = ::send(fd_, data, len, 0);
      if (n <= 0) return false;
      data += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }
 private:
  int fd_;
};

}  // namespace

bool PrintDocument(int socket_fd) {
  brhbp::JobSettings js;
  js.paper    = brhbp::Paper::kA4;
  js.dpi      = 600;
  js.duplex   = brhbp::Duplex::kLongEdge;
  js.job_name = "doc";

  SocketSink sink(socket_fd);
  brhbp::JobEncoder enc(&sink, js);
  const brhbp::PageGeometry& g = enc.geometry();

  // The entire buffer this function ever allocates: 64 rows, ~39 KB at 600
  // dpi.  Compare 139 MB for one ARGB_8888 A4 page at the same resolution.
  constexpr int kBandRows = 64;
  std::vector<uint8_t> band(static_cast<size_t>(kBandRows) * g.stride);

  if (!enc.Begin()) return false;

  for (int page = 0; page < PageCount(); ++page) {
    if (!enc.BeginPage()) return false;

    for (int row = 0; row < g.rows; row += kBandRows) {
      const int n = (g.rows - row < kBandRows) ? (g.rows - row) : kBandRows;
      if (!RenderBand(page, row, n, g.stride, band.data())) return false;
      for (int i = 0; i < n; ++i) {
        if (!enc.WriteRow(band.data() + static_cast<size_t>(i) * g.stride))
          return false;
      }
      // `band` is reused immediately.  Nothing accumulates.
    }

    if (!enc.EndPage()) return false;
    // Page `page` is now on the wire.  The engine begins printing it while
    // the loop goes back to render page `page + 1`.  Verified on hardware:
    // a page sent alone printed 14.3 s later with the next page still unsent.
  }

  return enc.End();
}
