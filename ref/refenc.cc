// Conformance harness: drive brlaser's own job.cc/line.cc (unmodified, no CUPS)
// with a PBM stream, so brpdf -x output can be compared byte-for-byte.
//   refenc JOBNAME PAPER DPI COPIES DUPLEX(0/1) < pages.pbm > ref.prn
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include "brlaser/job.h"

static FILE *in = stdin;
static std::vector<uint8_t> page; static size_t stride, row, rows;

static int pbm_int() {
  int c, v = 0;
  do { c = getc(in); if (c == '#') while (c != '\n' && c != EOF) c = getc(in); } while (isspace(c) || c == '#');
  while (isdigit(c)) { v = v * 10 + (c - '0'); c = getc(in); }
  return v;
}
static bool read_pbm(int &w, int &h) {
  int c; while ((c = getc(in)) != EOF && c != 'P') {}
  if (c == EOF) return false;
  getc(in); w = pbm_int(); h = pbm_int();
  stride = (w + 7) / 8; rows = h; row = 0;
  page.resize(stride * rows);
  return fread(page.data(), 1, page.size(), in) == page.size();
}
static bool next_line(std::vector<uint8_t> &buf) {
  if (row >= rows) return false;
  std::copy(page.begin() + row * stride, page.begin() + (row + 1) * stride, buf.begin());
  ++row; return true;
}

int main(int argc, char **argv) {
  if (argc != 6) { fprintf(stderr, "usage: refenc JOB PAPER DPI COPIES DUPLEX\n"); return 2; }
  page_params p = {};
  p.num_copies = atoi(argv[4]); p.resolution = atoi(argv[3]); p.duplex = atoi(argv[5]);
  p.economode = false; p.sourcetray = "AUTO"; p.mediatype = "PLAIN"; p.papersize = argv[2];
  job j(stdout, argv[1]);
  int w, h;
  while (read_pbm(w, h)) j.encode_page(p, h, (int)stride, next_line);
  return 0;
}
