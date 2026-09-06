// ippcancel_tool.cc -- exercise the out-of-band cancel operations.
#include "ippcancel.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  const char* host = "192.168.1.17";
  const char* res  = "/ipp/print";
  const char* user = "brhbp";
  const char* op   = "current";
  int port = 631, job = -1;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-h") && i + 1 < argc) host = argv[++i];
    else if (!strcmp(argv[i], "-P") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-r") && i + 1 < argc) res = argv[++i];
    else if (!strcmp(argv[i], "-u") && i + 1 < argc) user = argv[++i];
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) { job = atoi(argv[++i]); op = "job"; }
    else if (!strcmp(argv[i], "--purge")) op = "purge";
    else if (!strcmp(argv[i], "--current")) op = "current";
    else { fprintf(stderr, "usage: %s [-h host] [-P port] [-r res] [-u user] "
                           "[--current | --purge | -i JOBID]\n", argv[0]); return 2; }
  }
  brhbp::IppStatus s;
  if (!strcmp(op, "purge"))       s = brhbp::PurgeJobs(host, port, res, user);
  else if (!strcmp(op, "job"))    s = brhbp::CancelJob(host, port, res, user, job);
  else                            s = brhbp::CancelCurrentJob(host, port, res, user);
  printf("%-8s http=%d ipp=0x%04x %s\n", op, s.http_status, s.ipp_status, s.detail.c_str());
  return s.result == brhbp::IppResult::kOk ? 0 : 1;
}
