#include "status.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace brhbp {
namespace {

constexpr uint8_t kSequence   = 0x30;
constexpr uint8_t kInteger    = 0x02;
constexpr uint8_t kOctetStr   = 0x04;
constexpr uint8_t kNull       = 0x05;
constexpr uint8_t kOidTag     = 0x06;
constexpr uint8_t kGetRequest = 0xA0;
constexpr uint8_t kCounter32  = 0x41;
constexpr uint8_t kGauge32    = 0x42;
constexpr uint8_t kTimeTicks  = 0x43;

void PutLen(std::string& b, size_t n) {
  if (n < 0x80) { b.push_back(static_cast<char>(n)); return; }
  // Long form; two bytes covers anything we build.
  if (n < 0x100) { b.push_back(static_cast<char>(0x81)); b.push_back(static_cast<char>(n)); }
  else { b.push_back(static_cast<char>(0x82));
         b.push_back(static_cast<char>(n >> 8)); b.push_back(static_cast<char>(n & 0xff)); }
}
void PutTLV(std::string& b, uint8_t tag, const std::string& v) {
  b.push_back(static_cast<char>(tag)); PutLen(b, v.size()); b += v;
}
std::string EncInt(int32_t v) {
  uint8_t tmp[5]; int n = 0;
  if (v == 0) { tmp[n++] = 0; }
  else {
    bool neg = v < 0;
    uint32_t u = static_cast<uint32_t>(v);
    uint8_t bytes[4] = { static_cast<uint8_t>(u >> 24), static_cast<uint8_t>(u >> 16),
                         static_cast<uint8_t>(u >> 8),  static_cast<uint8_t>(u) };
    int i = 0;
    while (i < 3 && bytes[i] == (neg ? 0xff : 0x00) &&
           ((bytes[i + 1] & 0x80) == (neg ? 0x80 : 0x00))) ++i;
    for (; i < 4; ++i) tmp[n++] = bytes[i];
  }
  return std::string(reinterpret_cast<char*>(tmp), static_cast<size_t>(n));
}
bool EncOid(const char* dotted, std::string* out) {
  std::vector<uint32_t> arcs;
  const char* p = dotted;
  while (*p) {
    char* end = nullptr;
    unsigned long v = std::strtoul(p, &end, 10);
    if (end == p) return false;
    arcs.push_back(static_cast<uint32_t>(v));
    p = (*end == '.') ? end + 1 : end;
  }
  if (arcs.size() < 2) return false;
  out->clear();
  out->push_back(static_cast<char>(arcs[0] * 40 + arcs[1]));
  for (size_t i = 2; i < arcs.size(); ++i) {
    uint32_t v = arcs[i];
    uint8_t buf[5]; int n = 0;
    do { buf[n++] = static_cast<uint8_t>(v & 0x7f); v >>= 7; } while (v);
    for (int j = n - 1; j >= 0; --j)
      out->push_back(static_cast<char>(buf[j] | (j ? 0x80 : 0x00)));
  }
  return true;
}

// --- minimal BER walk over the response -----------------------------------
struct Cursor { const uint8_t* p; const uint8_t* end; };
bool ReadTL(Cursor& c, uint8_t* tag, size_t* len) {
  if (c.p >= c.end) return false;
  *tag = *c.p++;
  if (c.p >= c.end) return false;
  size_t n = *c.p++;
  if (n & 0x80) {
    const int k = n & 0x7f;
    if (k < 1 || k > 4 || c.p + k > c.end) return false;
    n = 0;
    for (int i = 0; i < k; ++i) n = (n << 8) | *c.p++;
  }
  if (c.p + n > c.end) return false;
  *len = n;
  return true;
}
bool ReadIntVal(const uint8_t* p, size_t n, int64_t* out) {
  if (n == 0 || n > 8) return false;
  int64_t v = (p[0] & 0x80) ? -1 : 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
  *out = v;
  return true;
}

bool ExtractVarbindValue(const uint8_t* buf, size_t n, uint8_t* tag, int64_t* ival,
                         const uint8_t** sval, size_t* slen) {
  Cursor c{buf, buf + n};
  uint8_t t; size_t l;
  if (!ReadTL(c, &t, &l) || t != kSequence) return false;      // message
  if (!ReadTL(c, &t, &l) || t != kInteger) return false;       // version
  c.p += l;
  if (!ReadTL(c, &t, &l) || t != kOctetStr) return false;      // community
  c.p += l;
  if (!ReadTL(c, &t, &l)) return false;                        // PDU (0xA2)
  if (!ReadTL(c, &t, &l) || t != kInteger) return false;       // request-id
  c.p += l;
  if (!ReadTL(c, &t, &l) || t != kInteger) return false;       // error-status
  int64_t err = 0; if (!ReadIntVal(c.p, l, &err)) return false;
  c.p += l;
  if (err != 0) return false;
  if (!ReadTL(c, &t, &l) || t != kInteger) return false;       // error-index
  c.p += l;
  if (!ReadTL(c, &t, &l) || t != kSequence) return false;      // varbind list
  if (!ReadTL(c, &t, &l) || t != kSequence) return false;      // varbind
  if (!ReadTL(c, &t, &l) || t != kOidTag) return false;        // name
  c.p += l;
  if (!ReadTL(c, &t, &l)) return false;                        // value
  *tag = t;
  if (t == kInteger || t == kCounter32 || t == kGauge32 || t == kTimeTicks)
    return ReadIntVal(c.p, l, ival);
  *sval = c.p; *slen = l;
  return true;
}

bool Query(const char* host, const char* community, const char* oid, int timeout_ms,
           uint8_t* tag, int64_t* ival, const uint8_t** sval, size_t* slen,
           std::vector<uint8_t>* scratch) {
  static int32_t rid = 1;
  std::string req;
  if (!EncodeGetRequest(community, oid, rid++, &req)) return false;

  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host, "161", &hints, &res) != 0 || !res) return false;
  int fd = ::socket(res->ai_family, SOCK_DGRAM, 0);
  if (fd < 0) { freeaddrinfo(res); return false; }
  struct timeval tv { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  bool ok = ::sendto(fd, req.data(), req.size(), 0, res->ai_addr, res->ai_addrlen) ==
            static_cast<ssize_t>(req.size());
  freeaddrinfo(res);
  if (ok) {
    scratch->resize(2048);
    ssize_t n = ::recv(fd, scratch->data(), scratch->size(), 0);
    ok = n > 0 && ExtractVarbindValue(scratch->data(), static_cast<size_t>(n),
                                      tag, ival, sval, slen);
  }
  ::close(fd);
  return ok;
}

}  // namespace

bool EncodeGetRequest(const char* community, const char* oid, int32_t request_id,
                      std::string* out) {
  std::string oid_enc;
  if (!EncOid(oid, &oid_enc)) return false;

  std::string varbind;
  PutTLV(varbind, kOidTag, oid_enc);
  PutTLV(varbind, kNull, std::string());
  std::string vb_seq; PutTLV(vb_seq, kSequence, varbind);
  std::string vb_list; PutTLV(vb_list, kSequence, vb_seq);

  std::string pdu;
  PutTLV(pdu, kInteger, EncInt(request_id));
  PutTLV(pdu, kInteger, EncInt(0));            // error-status
  PutTLV(pdu, kInteger, EncInt(0));            // error-index
  pdu += vb_list;

  std::string msg;
  PutTLV(msg, kInteger, EncInt(0));            // version 1 == 0
  PutTLV(msg, kOctetStr, std::string(community));
  PutTLV(msg, kGetRequest, pdu);

  out->clear();
  PutTLV(*out, kSequence, msg);
  return true;
}

DeviceStatus PollStatus(const char* host, const char* community, int timeout_ms) {
  DeviceStatus st;
  uint8_t tag; int64_t iv; const uint8_t* sv = nullptr; size_t sl = 0;
  std::vector<uint8_t> scratch;

  if (Query(host, community, "1.3.6.1.2.1.25.3.5.1.1.1", timeout_ms, &tag, &iv, &sv, &sl, &scratch)) {
    st.ok = true;
    switch (iv) {
      case 1: st.state = PrinterState::kOther; break;
      case 3: st.state = PrinterState::kIdle; break;
      case 4: st.state = PrinterState::kPrinting; break;
      case 5: st.state = PrinterState::kWarmup; break;
      default: st.state = PrinterState::kUnknown; break;
    }
  } else {
    st.detail = "no reply to hrPrinterStatus";
    return st;
  }
  if (Query(host, community, "1.3.6.1.2.1.25.3.5.1.2.1", timeout_ms, &tag, &iv, &sv, &sl, &scratch))
    if (sv && sl >= 1) st.errors = sv[0];
  if (Query(host, community, "1.3.6.1.2.1.43.10.2.1.4.1.1", timeout_ms, &tag, &iv, &sv, &sl, &scratch))
    st.life_count = iv;
  return st;
}

std::string DescribeErrors(uint16_t m) {
  if (!m) return "none";
  std::string s;
  auto add = [&](uint16_t bit, const char* n) {
    if (m & bit) { if (!s.empty()) s += ", "; s += n; } };
  add(kLowPaper, "low paper");  add(kNoPaper, "no paper");
  add(kLowToner, "low toner");  add(kNoToner, "no toner");
  add(kDoorOpen, "door open");  add(kJammed, "jammed");
  add(kOffline, "offline");     add(kServiceReq, "service requested");
  return s;
}

const char* StateName(PrinterState s) {
  switch (s) {
    case PrinterState::kIdle: return "idle";
    case PrinterState::kPrinting: return "printing";
    case PrinterState::kWarmup: return "warmup";
    case PrinterState::kOther: return "other";
    default: return "unknown";
  }
}

}  // namespace brhbp
