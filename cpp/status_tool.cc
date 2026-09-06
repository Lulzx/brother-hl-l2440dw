// status_tool.cc -- poll device status, the substitute for the back-channel
// port 9100 does not have.
#include "status.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main(int argc, char** argv) {
  const char* host = getenv("BRPRINTER") ? getenv("BRPRINTER") : "";
  const char* comm = "public";
  int watch = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-h") && i + 1 < argc) host = argv[++i];
    else if (!strcmp(argv[i], "-c") && i + 1 < argc) comm = argv[++i];
    else if (!strcmp(argv[i], "-w") && i + 1 < argc) watch = atoi(argv[++i]);
    else { fprintf(stderr, "usage: %s [-h host] [-c community] [-w seconds]\n", argv[0]); return 2; }
  }
  if (!*host) { fprintf(stderr, "set BRPRINTER or pass -h HOST\n"); return 2; }
  int64_t last = -1;
  for (;;) {
    brhbp::DeviceStatus s = brhbp::PollStatus(host, comm);
    if (!s.ok) {
      printf("unreachable: %s\n", s.detail.c_str());
    } else {
      printf("%-9s errors=%-22s impressions=%lld%s\n",
             brhbp::StateName(s.state), brhbp::DescribeErrors(s.errors).c_str(),
             static_cast<long long>(s.life_count),
             (last >= 0 && s.life_count > last) ? "  <- sheet out" : "");
      last = s.life_count;
    }
    if (!watch) break;
    sleep(static_cast<unsigned>(watch));
  }
  return 0;
}
