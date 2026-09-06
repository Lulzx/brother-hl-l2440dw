// brhbp_bridge.h -- flat C surface over the C++ engine, for Swift.
//
// Swift 6 imports C++ directly, and for a leaf type that would be fine. This
// is the app's ABI boundary though, and a flat C API is trivially stable,
// trivially callable from a Swift actor, and does not drag std::string across
// a module boundary. The novelty belongs in the UI layer, not here.
#ifndef BRHBP_BRIDGE_H
#define BRHBP_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BrhbpJob BrhbpJob;

typedef struct {
    int32_t width_px, rows, stride, origin_x, origin_y;
} BrhbpGeometry;

typedef struct {
    bool     ok;
    int32_t  state;        // 0 unknown, 1 other, 3 idle, 4 printing, 5 warmup
    uint32_t errors;       // bit mask
    int64_t  life_count;
} BrhbpStatus;

// --- encoding ---------------------------------------------------------------
// paper: 0 A4, 1 Letter, 2 Legal, 3 A5, 4 A6, 5 B5, 6 B6, 7 Executive,
//        8 C5, 9 DL, 10 Monarch
// duplex: 0 off, 1 long edge (back pages must be pre-rotated), 2 short edge.
BrhbpJob* brhbp_open_fd(int fd, int32_t paper, int32_t dpi, int32_t copies,
                        int32_t duplex, bool toner_save, const char* job_name);
BrhbpGeometry brhbp_geometry(BrhbpJob*);
bool brhbp_begin(BrhbpJob*);
bool brhbp_begin_page(BrhbpJob*);
bool brhbp_write_rows(BrhbpJob*, const uint8_t* rows, int32_t n_rows);
bool brhbp_end_page(BrhbpJob*);
bool brhbp_end(BrhbpJob*);
void brhbp_request_cancel(BrhbpJob*);   // any thread
bool brhbp_abort(BrhbpJob*);            // writing thread only
int32_t brhbp_status_code(BrhbpJob*);
void brhbp_close(BrhbpJob*);

// --- rendering (MuPDF, band at a time) --------------------------------------
typedef struct BrhbpDoc BrhbpDoc;
BrhbpDoc* brhbp_doc_open(const char* path);
int32_t   brhbp_doc_pages(BrhbpDoc*);
// Renders rows [y0, y0+n) of `page` into `dst` as 1bpp, MSB first, 1 = black.
// halftone: 0 Bayer, 1 threshold, 2 Floyd-Steinberg.
//
// `rotate180` must be set for the back side of a long-edge duplex sheet. The
// engine prints the back exactly as received and the sheet is flipped about
// its long edge, so the host has to pre-rotate. Forgetting it is invisible in
// simplex and prints every second page upside down in duplex.
bool      brhbp_doc_render_band(BrhbpDoc*, int32_t page, int32_t dpi,
                                int32_t paper, int32_t y0, int32_t n_rows,
                                int32_t halftone, bool rotate180, bool fit,
                                uint8_t* dst);
// 8-bit gray preview of a whole page at `scale` (points -> pixels).
bool      brhbp_doc_preview(BrhbpDoc*, int32_t page, float scale,
                            int32_t* out_w, int32_t* out_h, uint8_t** out_gray);

// As above, but sized so the longer edge is `max_edge` pixels regardless of
// how big the page is. A fixed dpi renders an A0 drawing at twenty times the
// pixels of an A4 page for the same on-screen thumbnail; this does not.
bool      brhbp_doc_preview_fit(BrhbpDoc*, int32_t page, int32_t max_edge,
                                int32_t* out_w, int32_t* out_h, uint8_t** out_gray);
void      brhbp_free(void*);
void      brhbp_doc_close(BrhbpDoc*);

// --- status / cancel --------------------------------------------------------
BrhbpStatus brhbp_poll_status(const char* host, const char* community, int32_t timeout_ms);
int32_t     brhbp_ipp_cancel_current(const char* host, int32_t port, const char* resource,
                                     const char* user);
int32_t     brhbp_ipp_purge(const char* host, int32_t port, const char* resource,
                            const char* user);

#ifdef __cplusplus
}
#endif
#endif
