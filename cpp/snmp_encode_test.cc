// Prints our SNMPv1 GetRequest as hex, so it can be diffed against the packet
// net-snmp actually puts on the wire for the same community/OID/request-id.
#include "status.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s COMMUNITY OID REQID\n", argv[0]); return 2; }
  std::string out;
  if (!brhbp::EncodeGetRequest(argv[1], argv[2], atoi(argv[3]), &out)) {
    fprintf(stderr, "encode failed\n"); return 1;
  }
  for (unsigned char c : out) printf("%02x", c);
  printf("\n");
  return 0;
}
