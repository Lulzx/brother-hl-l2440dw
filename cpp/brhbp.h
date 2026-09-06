// brhbp.h -- streaming encoder for Brother host-based ("HBP") mono lasers.
//
// Design goal: constant memory, page-at-a-time output, so a phone can start
// printing sheet 1 while sheet 2 is still being rendered.  The engine takes
// ~14 s per sheet; anything you can do in under that is free.
//
// The whole interface is built around one property of the wire format: bands
// are self-contained.  A band never references data in a previous band, so a
// page can be emitted 64 rows at a time and the rows discarded immediately.
// Peak working set is one reference row plus one band, not one page:
//
//     A4 @600 dpi   full page ARGB_8888  139 MB
//                   full page 1-bit        4.4 MB
//                   this encoder          ~55 KB
//
// Threading: an instance is not thread-safe.  Use one per job.  For parallel
// encoding see BandEncoder at the bottom, and read the caveat attached to it.

#ifndef BRHBP_H_
#define BRHBP_H_

#include <cstddef>
#include <cstdint>

namespace brhbp {

// ---------------------------------------------------------------------------
// Output

// Where encoded bytes go: a socket, a file, a buffer.  Implementations must
// not retain `data` past the call.  Return false to abort the job; the
// encoder will stop and surface it through Status.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool Write(const uint8_t* data, size_t len) = 0;
};

enum class Status {
  kOk = 0,
  kSinkError,        // Sink::Write returned false
  kBadArgument,      // unsupported dpi/paper, null row, wrong call order
  kTooManyRows,      // WriteRow called more than PageGeometry::rows times
};

// ---------------------------------------------------------------------------
// Job and page settings.  These map 1:1 onto PJL lines; see FORMAT.md s2.

enum class Paper  { kA4, kLetter, kLegal, kA5, kA6, kB5, kB6, kExecutive,
                    kC5, kDL, kMonarch };
enum class Media  { kPlain, kThin, kThick, kThicker, kBond, kTrans, kEnv,
                    kEnvThick, kEnvThin };
enum class Tray   { kAuto, kT1, kT2, kT3, kMP, kManual };
enum class Duplex { kNone, kLongEdge };

struct JobSettings {
  Paper  paper       = Paper::kA4;
  int    dpi         = 600;          // 300, 600 or 1200
  int    copies      = 1;            // 1..999
  Duplex duplex      = Duplex::kNone;
  Media  media       = Media::kPlain;
  Tray   tray        = Tray::kAuto;
  bool   toner_save  = false;
  const char* job_name = "job";      // ASCII, <=79 chars, no quote/backslash
};

// Everything the caller needs to produce rows of the right shape.  Derived
// from paper + dpi and the engine's unprintable margin, so the caller never
// has to know the margin model.
struct PageGeometry {
  int    width_px;    // printable width in pixels
  int    rows;        // printable height in rows
  size_t stride;      // bytes per row = (width_px + 7) / 8
  int    origin_x;    // offset of the printable area within the full sheet,
  int    origin_y;    //   in pixels, if the caller renders a whole sheet
};

// Compute geometry without constructing an encoder (useful for sizing a
// rasteriser's clip rect before you commit to printing).
PageGeometry ComputeGeometry(Paper paper, int dpi);

// ---------------------------------------------------------------------------
// The streaming encoder.
//
// Call order:
//
//     Begin()
//       BeginPage()  WriteRow() x rows  EndPage()      // page 1
//       BeginPage()  WriteRow() x rows  EndPage()      // page 2
//       ...
//     End()
//
// Rows are 1 bit per pixel, MSB first, 1 = black -- identical to PBM (P4).
// Each row must be exactly PageGeometry::stride bytes and already cropped to
// the printable area.  The encoder copies what it needs before returning, so
// the caller may reuse or free the row buffer immediately.  That is the whole
// point: the caller can render, halftone and discard one band at a time.
class JobEncoder {
 public:
  JobEncoder(Sink* sink, const JobSettings& settings);
  ~JobEncoder();

  JobEncoder(const JobEncoder&) = delete;
  JobEncoder& operator=(const JobEncoder&) = delete;

  const PageGeometry& geometry() const { return geom_; }
  Status status() const { return status_; }

  bool Begin();                    // PJL envelope + job header
  bool BeginPage();                // per-page PJL/PCL header, opens the raster
  bool WriteRow(const uint8_t* row);
  bool EndPage();                  // flush partial band, terminator, form feed
  bool End();                      // EOJ + UEL

  // Rows still expected on the current page.  A caller that runs out of
  // content early can just call EndPage(); the engine leaves the rest white.
  int rows_remaining() const { return geom_.rows - row_index_; }

 private:
  bool FlushBand();
  void EncodeRowAgainstRef(const uint8_t* row);
  void EncodeRowNoRef(const uint8_t* row);

  Sink*        sink_;
  JobSettings  settings_;
  PageGeometry geom_;
  Status       status_ = Status::kOk;

  // --- the entire per-job working set ---------------------------------------
  // ref_   : previous decoded row, the delta reference          (stride)
  // line_  : scratch for one encoded row                        (see note)
  // band_  : accumulated encoded rows awaiting flush            (16350 + slack)
  //
  // A single encoded row is bounded by stride + 254 * ~4 + slack: at most 254
  // edits, each costing one opcode byte plus a bounded overflow prefix, and
  // the literal payload across all edits cannot exceed stride.  kLineSlack
  // covers that with room to spare.
  static constexpr size_t kMaxBand   = 16350;   // format cap, FORMAT.md s4
  static constexpr size_t kLineSlack = 2048;

  uint8_t* ref_   = nullptr;
  uint8_t* line_  = nullptr;
  uint8_t* band_  = nullptr;
  size_t   line_len_ = 0;
  size_t   band_len_ = 0;
  int      band_rows_ = 0;
  int      row_index_ = 0;
  bool     page_open_ = false;
};

// ---------------------------------------------------------------------------
// Parallel band encoding.
//
// Because a band never references another band, bands can be encoded on
// separate threads and concatenated in order.  This encodes exactly `n_rows`
// as one self-contained band and returns its length, or 0 if it does not fit.
//
// CAVEAT, read before using: JobEncoder starts a new band every 64 rows *or*
// when the current one would exceed kMaxBand, whichever comes first.  Fixing
// band boundaries at 64 rows so they can be farmed out changes where the
// splits land on dense pages.  The result is still a valid job -- the engine
// only cares that each band is self-contained -- but it is no longer
// byte-identical to brlaser's output, which is what the conformance test in
// test/roundtrip.sh anchors on.  Keep the serial path for anything you want
// to verify against the reference encoder.
size_t EncodeBand(const uint8_t* rows,      // n_rows * stride, contiguous
                  int n_rows,               // <= 64
                  size_t stride,
                  uint8_t* out,
                  size_t out_capacity);

}  // namespace brhbp

#endif  // BRHBP_H_
