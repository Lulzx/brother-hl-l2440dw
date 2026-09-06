// brhbp.cc -- streaming encoder for Brother host-based ("HBP") mono lasers.
//
// Output is byte-identical to brpdf.c for the same input, which is what keeps
// the conformance chain in test/roundtrip.sh meaningful: brpdf is verified
// byte-for-byte against brlaser, so anything identical to brpdf inherits that.
// Every optimisation here changes only how fast the same answer is found.
//
// The three hot loops -- common-prefix skip, common-suffix trim and run
// length -- are word-at-a-time rather than byte-at-a-time. On a text page most
// of a scanline is identical to the one above it, so the prefix skip is where
// the time actually goes.

#include "brhbp.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace brhbp {
namespace {

constexpr int kMarginL = 8, kMarginR = 8, kMarginT = 16, kMarginB = 8;
constexpr int kLinesPerBand = 64;

struct PaperDim { Paper id; const char* name; int w_pt, h_pt; };
constexpr PaperDim kPapers[] = {
    {Paper::kA4,        "A4",        595,  842},
    {Paper::kLetter,    "LETTER",    612,  792},
    {Paper::kLegal,     "LEGAL",     612, 1008},
    {Paper::kA5,        "A5",        420,  595},
    {Paper::kA6,        "A6",        298,  420},
    {Paper::kB5,        "B5",        516,  729},
    {Paper::kB6,        "B6",        363,  516},
    {Paper::kExecutive, "EXECUTIVE", 522,  756},
    {Paper::kC5,        "C5",        459,  649},
    {Paper::kDL,        "DL",        312,  624},
    {Paper::kMonarch,   "MONARCH",   279,  540},
};

const PaperDim& Dim(Paper p) {
  for (const PaperDim& d : kPapers) if (d.id == p) return d;
  return kPapers[0];
}
int Pt2Px(int pt, int dpi) { return pt * dpi / 72; }

const char* MediaName(Media m) {
  switch (m) {
    case Media::kPlain:    return "PLAIN";
    case Media::kThin:     return "THIN";
    case Media::kThick:    return "THICK";
    case Media::kThicker:  return "THICKER";
    case Media::kBond:     return "BOND";
    case Media::kTrans:    return "TRANS";
    case Media::kEnv:      return "ENV";
    case Media::kEnvThick: return "ENV-THICK";
    case Media::kEnvThin:  return "ENV-THIN";
  }
  return "PLAIN";
}
const char* TrayName(Tray t) {
  switch (t) {
    case Tray::kAuto:   return "AUTO";
    case Tray::kT1:     return "T1";
    case Tray::kT2:     return "T2";
    case Tray::kT3:     return "T3";
    case Tray::kMP:     return "MP";
    case Tray::kManual: return "MANUAL";
  }
  return "AUTO";
}

// --- word-at-a-time primitives ---------------------------------------------
// Little-endian only; both arm64 and x86_64 qualify. The scalar fallbacks
// below are what a big-endian build would use.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BRHBP_LE 1
#endif

inline uint64_t Load64(const uint8_t* p) {
  uint64_t v; std::memcpy(&v, p, 8); return v;
}

// Index of the first byte where a and b differ, or n if identical.
inline int CommonPrefix(const uint8_t* a, const uint8_t* b, int n) {
  int i = 0;
#ifdef BRHBP_LE
  for (; i + 8 <= n; i += 8) {
    uint64_t x = Load64(a + i) ^ Load64(b + i);
    if (x) return i + (__builtin_ctzll(x) >> 3);
  }
#endif
  for (; i < n; ++i) if (a[i] != b[i]) break;
  return i;
}

// Length of the run of bytes equal to p[0], starting at p, capped at n.
inline int RunLength(const uint8_t* p, int n) {
  if (n <= 0) return 0;
  int i = 1;
#ifdef BRHBP_LE
  const uint64_t bcast = 0x0101010101010101ULL * p[0];
  for (; i + 8 <= n; i += 8) {
    uint64_t x = Load64(p + i) ^ bcast;
    if (x) return i + (__builtin_ctzll(x) >> 3);
  }
#endif
  for (; i < n; ++i) if (p[i] != p[0]) break;
  return i;
}

inline bool AllZero(const uint8_t* p, int n) {
  int i = 0;
#ifdef BRHBP_LE
  for (; i + 8 <= n; i += 8) if (Load64(p + i)) return false;
#endif
  for (; i < n; ++i) if (p[i]) return false;
  return true;
}

// How many literal bytes to emit before the row re-synchronises with the
// reference (two consecutive matching bytes) or a run of three begins.
// Inherently a 3-wide sliding window; kept scalar on purpose.
inline int SubstituteLength(const uint8_t* l, const uint8_t* r, int n) {
  if (n <= 0) return 0;
  int it = 0, next = 1, prev = 0;
  while (next < n) {
    if (l[it] == r[it] && l[next] == r[next]) return it;
    if (l[it] == l[next] && l[it] == l[prev]) return prev;
    prev = it; it = next; ++next;
  }
  return n;
}

inline int IMin(int a, int b) { return a < b ? a : b; }

}  // namespace

PageGeometry ComputeGeometry(Paper paper, int dpi) {
  const PaperDim& d = Dim(paper);
  PageGeometry g{};
  g.width_px = Pt2Px(d.w_pt - kMarginL - kMarginR, dpi);
  g.rows     = Pt2Px(d.h_pt - kMarginT - kMarginB, dpi);
  g.stride   = static_cast<size_t>((g.width_px + 7) / 8);
  g.origin_x = Pt2Px(kMarginL, dpi);
  g.origin_y = Pt2Px(kMarginT, dpi);
  return g;
}

// ---------------------------------------------------------------------------

JobEncoder::JobEncoder(Sink* sink, const JobSettings& settings)
    : sink_(sink), settings_(settings) {
  geom_ = ComputeGeometry(settings.paper, settings.dpi);
  ref_  = static_cast<uint8_t*>(std::calloc(1, geom_.stride));
  line_ = static_cast<uint8_t*>(std::malloc(geom_.stride + kLineSlack));
  band_ = static_cast<uint8_t*>(std::malloc(kMaxBand + geom_.stride + kLineSlack));
  if (!ref_ || !line_ || !band_) status_ = Status::kBadArgument;
}

JobEncoder::~JobEncoder() { std::free(ref_); std::free(line_); std::free(band_); }

bool JobEncoder::Emit(const void* p, size_t n) {
  if (status_ != Status::kOk) return false;
  if (!sink_->Write(static_cast<const uint8_t*>(p), n)) {
    status_ = Status::kSinkError;
    return false;
  }
  return true;
}

bool JobEncoder::EmitF(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0 || static_cast<size_t>(n) >= sizeof buf) {
    status_ = Status::kBadArgument;
    return false;
  }
  return Emit(buf, static_cast<size_t>(n));
}

bool JobEncoder::Begin() {
  if (status_ != Status::kOk) return false;
  static const uint8_t kPad[128] = {0};
  if (!Emit(kPad, sizeof kPad)) return false;           // wake-up padding
  if (!EmitF("\033%%-12345X@PJL\n")) return false;
  return EmitF("@PJL JOB NAME=\"%s\"\n", settings_.job_name);
}

bool JobEncoder::BeginPage() {
  if (status_ != Status::kOk || page_open_) return false;
  // brpdf/brlaser emit the settings block once per job, not per page, because
  // the settings do not change within a job. Matching that is what keeps the
  // output byte-identical on multi-page documents.
  if (page_header_done_) {
    std::memset(ref_, 0, geom_.stride);
    band_len_ = 0; band_rows_ = 0; row_index_ = 0;
    page_open_ = true; raster_open_ = false;
    return true;
  }
  page_header_done_ = true;
  if (!EmitF("\033%%-12345X@PJL\n")) return false;
  if (settings_.dpi == 1200) {
    if (!EmitF("@PJL SET RAS1200MODE = TRUE\n")) return false;
    if (!EmitF("@PJL SET RESOLUTION = 600\n")) return false;
  } else {
    if (!EmitF("@PJL SET RAS1200MODE = FALSE\n")) return false;
    if (!EmitF("@PJL SET RESOLUTION = %d\n", settings_.dpi)) return false;
  }
  if (!EmitF("@PJL SET ECONOMODE = %s\n", settings_.toner_save ? "ON" : "OFF")) return false;
  if (!EmitF("@PJL SET SOURCETRAY = %s\n", TrayName(settings_.tray))) return false;
  if (!EmitF("@PJL SET MEDIATYPE = %s\n", MediaName(settings_.media))) return false;
  if (!EmitF("@PJL SET PAPER = %s\n", Dim(settings_.paper).name)) return false;
  if (!EmitF("@PJL SET PAGEPROTECT = AUTO\n")) return false;
  if (!EmitF("@PJL SET ORIENTATION = PORTRAIT\n")) return false;
  if (!EmitF("@PJL ENTER LANGUAGE = PCL\n")) return false;
  if (!EmitF("\033E")) return false;                          // PCL reset
  if (!EmitF("\033&l%dX", settings_.copies)) return false;    // copies
  if (settings_.duplex == Duplex::kLongEdge && !EmitF("\033&l2S")) return false;

  std::memset(ref_, 0, geom_.stride);
  band_len_ = 0; band_rows_ = 0; row_index_ = 0;
  page_open_ = true;
  raster_open_ = false;
  return true;
}

// --- row encoders (exact ports of brpdf.c) ---------------------------------

void JobEncoder::PutOverflow(int v) {
  if (v < 0) return;
  if (v < 255) { line_[line_len_++] = static_cast<uint8_t>(v); return; }
  for (int i = 0; i < v / 255; ++i) line_[line_len_++] = 255;
  line_[line_len_++] = static_cast<uint8_t>(v % 255);
}

void JobEncoder::PutSubstitute(int offset, const uint8_t* p, int len) {
  const int count = len - 1;
  line_[line_len_++] =
      static_cast<uint8_t>((IMin(offset, 15) << 3) | IMin(count, 7));
  PutOverflow(offset - 15);
  PutOverflow(count - 7);
  std::memcpy(line_ + line_len_, p, static_cast<size_t>(len));
  line_len_ += static_cast<size_t>(len);
}

void JobEncoder::PutRepeat(int offset, int count, uint8_t value) {
  count -= 2;
  line_[line_len_++] =
      static_cast<uint8_t>(128 | (IMin(offset, 3) << 5) | IMin(count, 31));
  PutOverflow(offset - 3);
  PutOverflow(count - 31);
  line_[line_len_++] = value;
}

void JobEncoder::EncodeRowNoRef(const uint8_t* row) {
  const int n = static_cast<int>(geom_.stride);
  line_len_ = 0;
  if (AllZero(row, n)) { line_[line_len_++] = 0xFF; return; }
  line_[line_len_++] = 1;
  PutSubstitute(0, row, n);
}

void JobEncoder::EncodeRowRef(const uint8_t* row) {
  const int n = static_cast<int>(geom_.stride);
  line_len_ = 0;
  if (AllZero(row, n)) { line_[line_len_++] = 0xFF; return; }
  const size_t count_pos = line_len_;
  line_[line_len_++] = 0;                       // edit count, patched below
  int end = n;
  while (end > 0 && row[end - 1] == ref_[end - 1]) --end;
  int i = 0, edits = 0;
  for (;;) {
    const int start = i;
    i += CommonPrefix(row + i, ref_ + i, end - i);
    const int off = i - start;
    if (i == end) break;
    if (++edits == 254) { PutSubstitute(off, row + i, end - i); break; }
    const int s = SubstituteLength(row + i, ref_ + i, end - i);
    if (s > 0) { PutSubstitute(off, row + i, s); i += s; }
    else {
      const int r = RunLength(row + i, end - i);
      PutRepeat(off, r, row[i]);
      i += r;
    }
  }
  line_[count_pos] = static_cast<uint8_t>(edits);
}

bool JobEncoder::FlushBand() {
  if (!band_rows_) return true;
  if (!EmitF("%zuw", band_len_ + 2)) return false;
  const uint8_t hdr[2] = {0, static_cast<uint8_t>(band_rows_)};
  if (!Emit(hdr, 2)) return false;
  if (!Emit(band_, band_len_)) return false;
  band_len_ = 0; band_rows_ = 0;
  return true;
}

bool JobEncoder::WriteRow(const uint8_t* row) {
  if (status_ != Status::kOk || !page_open_) return false;
  if (row_index_ >= geom_.rows) { status_ = Status::kTooManyRows; return false; }

  if (row_index_ == 0) {
    EncodeRowNoRef(row);
  } else if (row_index_ % kLinesPerBand == 0) {
    if (!FlushBand()) return false;
    EncodeRowNoRef(row);
  } else {
    EncodeRowRef(row);
    if (band_len_ + line_len_ >= kMaxBand) {       // brpdf: !(blk.n + sz < MAX)
      if (!FlushBand()) return false;
      EncodeRowNoRef(row);
    }
  }
  std::memcpy(band_ + band_len_, line_, line_len_);
  band_len_ += line_len_;
  ++band_rows_;
  std::memcpy(ref_, row, geom_.stride);
  ++row_index_;

  // brpdf emits the compression-mode selector once, after the first row is
  // encoded but before any band reaches the sink.
  if (row_index_ == 1 && !raster_open_) {
    raster_open_ = true;
    return EmitF("\033*b1030m");
  }
  return true;
}

bool JobEncoder::EndPage() {
  if (status_ != Status::kOk || !page_open_) return false;
  if (!raster_open_ && !EmitF("\033*b1030m")) return false;
  if (!FlushBand()) return false;
  if (!EmitF("1030M\f")) return false;
  page_open_ = false;
  return true;
}

bool JobEncoder::End() {
  if (status_ != Status::kOk) return false;
  if (!EmitF("\033%%-12345X@PJL\n")) return false;
  if (!EmitF("@PJL EOJ NAME=\"%s\"\n", settings_.job_name)) return false;
  return EmitF("\033%%-12345X\n");
}

}  // namespace brhbp
