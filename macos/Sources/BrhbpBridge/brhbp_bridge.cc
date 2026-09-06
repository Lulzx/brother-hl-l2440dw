#include "include/brhbp_bridge.h"

#include "brhbp.h"
#include "ippcancel.h"
#include "status.h"

#include <mupdf/fitz.h>

#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace {

class FdSink final : public brhbp::Sink {
 public:
  explicit FdSink(int fd) : fd_(fd) {}
  bool Write(const uint8_t* d, size_t n) override {
    while (n) {
      ssize_t k = ::write(fd_, d, n);
      if (k < 0) { if (errno == EINTR) continue; return false; }
      if (k == 0) return false;
      d += k; n -= static_cast<size_t>(k);
    }
    return true;
  }
 private:
  int fd_;
};

brhbp::Paper PaperOf(int32_t v) {
  using P = brhbp::Paper;
  static const P kAll[] = { P::kA4, P::kLetter, P::kLegal, P::kA5, P::kA6, P::kB5,
                            P::kB6, P::kExecutive, P::kC5, P::kDL, P::kMonarch };
  if (v < 0 || v >= static_cast<int32_t>(sizeof kAll / sizeof *kAll)) return P::kA4;
  return kAll[v];
}

const uint8_t kBayer[64] = {
   0,32, 8,40, 2,34,10,42,  48,16,56,24,50,18,58,26,
  12,44, 4,36,14,46, 6,38,  60,28,52,20,62,30,54,22,
   3,35,11,43, 1,33, 9,41,  51,19,59,27,49,17,57,25,
  15,47, 7,39,13,45, 5,37,  63,31,55,23,61,29,53,21,
};

}  // namespace

struct BrhbpJob {
  std::unique_ptr<FdSink> sink;
  std::unique_ptr<brhbp::JobEncoder> enc;
};

struct BrhbpDoc {
  fz_context* ctx = nullptr;
  fz_document* doc = nullptr;
  // Error diffusion carries residue into the next row, so mode 2 requires
  // bands to be requested in order. Reset when y0 == 0.
  std::vector<int32_t> err_cur, err_next;
};

extern "C" {

BrhbpJob* brhbp_open_fd(int fd, int32_t paper, int32_t dpi, int32_t copies,
                        bool duplex, bool toner_save, const char* job_name) {
  auto* j = new BrhbpJob;
  j->sink = std::make_unique<FdSink>(fd);
  brhbp::JobSettings js;
  js.paper      = PaperOf(paper);
  js.dpi        = dpi;
  js.copies     = copies;
  js.duplex     = duplex ? brhbp::Duplex::kLongEdge : brhbp::Duplex::kNone;
  js.toner_save = toner_save;
  js.job_name   = job_name ? job_name : "job";
  j->enc = std::make_unique<brhbp::JobEncoder>(j->sink.get(), js);
  return j;
}

BrhbpGeometry brhbp_geometry(BrhbpJob* j) {
  const brhbp::PageGeometry& g = j->enc->geometry();
  return BrhbpGeometry{ g.width_px, g.rows, static_cast<int32_t>(g.stride),
                        g.origin_x, g.origin_y };
}

bool brhbp_begin(BrhbpJob* j)      { return j->enc->Begin(); }
bool brhbp_begin_page(BrhbpJob* j) { return j->enc->BeginPage(); }
bool brhbp_end_page(BrhbpJob* j)   { return j->enc->EndPage(); }
bool brhbp_end(BrhbpJob* j)        { return j->enc->End(); }
void brhbp_request_cancel(BrhbpJob* j) { j->enc->RequestCancel(); }
bool brhbp_abort(BrhbpJob* j)      { return j->enc->Abort(); }
int32_t brhbp_status_code(BrhbpJob* j) { return static_cast<int32_t>(j->enc->status()); }
void brhbp_close(BrhbpJob* j)      { delete j; }

bool brhbp_write_rows(BrhbpJob* j, const uint8_t* rows, int32_t n) {
  const size_t stride = j->enc->geometry().stride;
  for (int32_t i = 0; i < n; ++i)
    if (!j->enc->WriteRow(rows + static_cast<size_t>(i) * stride)) return false;
  return true;
}

BrhbpDoc* brhbp_doc_open(const char* path) {
  auto* d = new BrhbpDoc;
  d->ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
  if (!d->ctx) { delete d; return nullptr; }
  fz_register_document_handlers(d->ctx);
  fz_try(d->ctx) d->doc = fz_open_document(d->ctx, path);
  fz_catch(d->ctx) { fz_drop_context(d->ctx); delete d; return nullptr; }
  return d;
}

int32_t brhbp_doc_pages(BrhbpDoc* d) {
  int32_t n = 0;
  fz_try(d->ctx) n = fz_count_pages(d->ctx, d->doc);
  fz_catch(d->ctx) n = 0;
  return n;
}

bool brhbp_doc_render_band(BrhbpDoc* d, int32_t pno, int32_t dpi, int32_t paper,
                           int32_t y0, int32_t n_rows, int32_t halftone, uint8_t* dst) {
  brhbp::PageGeometry g = brhbp::ComputeGeometry(PaperOf(paper), dpi);
  const size_t stride = g.stride;
  std::memset(dst, 0, stride * static_cast<size_t>(n_rows));

  if (halftone == 2) {
    if (y0 == 0 || d->err_cur.size() != static_cast<size_t>(g.width_px) + 2) {
      d->err_cur.assign(g.width_px + 2, 0);
      d->err_next.assign(g.width_px + 2, 0);
    }
  }

  fz_page* page = nullptr;
  bool ok = true;
  fz_try(d->ctx) page = fz_load_page(d->ctx, d->doc, pno);
  fz_catch(d->ctx) return false;

  const float scale = dpi / 72.0f;
  const fz_rect bounds = fz_bound_page(d->ctx, page);
  fz_matrix m = fz_scale(scale, scale);
  m = fz_concat(m, fz_translate(-bounds.x0 * scale - g.origin_x,
                                -bounds.y0 * scale - g.origin_y));

  fz_irect bbox = { 0, y0, g.width_px, y0 + n_rows };
  fz_pixmap* pix = nullptr;
  fz_try(d->ctx) {
    pix = fz_new_pixmap_with_bbox(d->ctx, fz_device_gray(d->ctx), bbox, nullptr, 0);
    fz_clear_pixmap_with_value(d->ctx, pix, 0xff);
    fz_device* dev = fz_new_draw_device(d->ctx, fz_identity, pix);
    fz_try(d->ctx) fz_run_page(d->ctx, page, dev, m, nullptr);
    fz_always(d->ctx) { fz_close_device(d->ctx, dev); fz_drop_device(d->ctx, dev); }
    fz_catch(d->ctx) fz_rethrow(d->ctx);
  }
  fz_catch(d->ctx) { if (pix) fz_drop_pixmap(d->ctx, pix); fz_drop_page(d->ctx, page); return false; }

  const uint8_t* s = fz_pixmap_samples(d->ctx, pix);
  const int st = fz_pixmap_stride(d->ctx, pix);
  for (int32_t r = 0; r < n_rows; ++r) {
    const uint8_t* src = s + static_cast<size_t>(r) * st;
    uint8_t* row = dst + static_cast<size_t>(r) * stride;
    if (halftone == 2) {
      d->err_cur.swap(d->err_next);
      std::fill(d->err_next.begin(), d->err_next.end(), 0);
      for (int x = 0; x < g.width_px; ++x) {
        const int old = src[x] + d->err_cur[x + 1];
        const int nv = old < 128 ? 0 : 255;
        if (nv == 0) row[x >> 3] |= 0x80 >> (x & 7);
        const int e = old - nv;
        d->err_cur [x + 2] += e * 7 / 16;
        d->err_next[x    ] += e * 3 / 16;
        d->err_next[x + 1] += e * 5 / 16;
        d->err_next[x + 2] += e * 1 / 16;
      }
    } else if (halftone == 1) {
      for (int x = 0; x < g.width_px; ++x)
        if (src[x] < 128) row[x >> 3] |= 0x80 >> (x & 7);
    } else {
      const uint8_t* b = kBayer + (((y0 + r) & 7) << 3);
      for (int x = 0; x < g.width_px; ++x)
        if (src[x] <= (b[x & 7] * 255 + 32) / 64) row[x >> 3] |= 0x80 >> (x & 7);
    }
  }
  fz_drop_pixmap(d->ctx, pix);
  fz_drop_page(d->ctx, page);
  return ok;
}

bool brhbp_doc_preview(BrhbpDoc* d, int32_t pno, float scale,
                       int32_t* out_w, int32_t* out_h, uint8_t** out_gray) {
  fz_page* page = nullptr;
  fz_try(d->ctx) page = fz_load_page(d->ctx, d->doc, pno);
  fz_catch(d->ctx) return false;
  fz_pixmap* pix = nullptr;
  fz_try(d->ctx) {
    pix = fz_new_pixmap_from_page(d->ctx, page, fz_scale(scale, scale),
                                  fz_device_gray(d->ctx), 0);
  }
  fz_catch(d->ctx) { fz_drop_page(d->ctx, page); return false; }
  const int w = fz_pixmap_width(d->ctx, pix), h = fz_pixmap_height(d->ctx, pix);
  const int st = fz_pixmap_stride(d->ctx, pix);
  auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * h));
  if (buf) {
    const uint8_t* s = fz_pixmap_samples(d->ctx, pix);
    for (int y = 0; y < h; ++y)
      std::memcpy(buf + static_cast<size_t>(y) * w, s + static_cast<size_t>(y) * st,
                  static_cast<size_t>(w));
  }
  fz_drop_pixmap(d->ctx, pix);
  fz_drop_page(d->ctx, page);
  if (!buf) return false;
  *out_w = w; *out_h = h; *out_gray = buf;
  return true;
}

void brhbp_free(void* p) { std::free(p); }

void brhbp_doc_close(BrhbpDoc* d) {
  if (!d) return;
  if (d->doc) fz_drop_document(d->ctx, d->doc);
  if (d->ctx) fz_drop_context(d->ctx);
  delete d;
}

BrhbpStatus brhbp_poll_status(const char* host, const char* community, int32_t timeout_ms) {
  brhbp::DeviceStatus s = brhbp::PollStatus(host, community ? community : "public", timeout_ms);
  return BrhbpStatus{ s.ok, static_cast<int32_t>(s.state), s.errors, s.life_count };
}

int32_t brhbp_ipp_cancel_current(const char* host, int32_t port, const char* res,
                                 const char* user) {
  return brhbp::CancelCurrentJob(host, port, res, user).ipp_status;
}
int32_t brhbp_ipp_purge(const char* host, int32_t port, const char* res, const char* user) {
  return brhbp::PurgeJobs(host, port, res, user).ipp_status;
}

}  // extern "C"
