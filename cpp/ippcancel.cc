#include "ippcancel.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cctype>
#include <cstring>
#include <vector>

namespace brhbp {
namespace {

// --- IPP encoding ----------------------------------------------------------
constexpr uint8_t kOperationAttributesTag = 0x01;
constexpr uint8_t kEndOfAttributesTag     = 0x03;
constexpr uint8_t kIntegerTag             = 0x21;
constexpr uint8_t kUriTag                 = 0x45;
constexpr uint8_t kNameWithoutLanguageTag = 0x42;
constexpr uint8_t kCharsetTag             = 0x47;
constexpr uint8_t kNaturalLanguageTag     = 0x48;

constexpr uint16_t kOpCancelJob        = 0x0008;
constexpr uint16_t kOpPurgeJobs        = 0x0012;
constexpr uint16_t kOpCancelCurrentJob = 0x0025;

void Put16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(v >> 8); b.push_back(v & 0xff);
}
void Put32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v);
}
void PutStr(std::vector<uint8_t>& b, const char* s) {
  const uint16_t n = static_cast<uint16_t>(std::strlen(s));
  Put16(b, n);
  b.insert(b.end(), s, s + n);
}
void PutAttr(std::vector<uint8_t>& b, uint8_t tag, const char* name, const char* value) {
  b.push_back(tag); PutStr(b, name); PutStr(b, value);
}
void PutIntAttr(std::vector<uint8_t>& b, const char* name, int32_t value) {
  b.push_back(kIntegerTag); PutStr(b, name); Put16(b, 4); Put32(b, static_cast<uint32_t>(value));
}

std::vector<uint8_t> BuildRequest(uint16_t op, const char* uri, const char* user,
                                  int job_id /* <0 = omit */) {
  static uint32_t request_id = 1;
  std::vector<uint8_t> b;
  b.push_back(2); b.push_back(0);              // IPP 2.0
  Put16(b, op);
  Put32(b, request_id++);
  b.push_back(kOperationAttributesTag);
  PutAttr(b, kCharsetTag,         "attributes-charset",          "utf-8");
  PutAttr(b, kNaturalLanguageTag, "attributes-natural-language", "en");
  PutAttr(b, kUriTag,             "printer-uri",                  uri);
  if (job_id >= 0) PutIntAttr(b, "job-id", job_id);
  PutAttr(b, kNameWithoutLanguageTag, "requesting-user-name", user);
  b.push_back(kEndOfAttributesTag);
  return b;
}

bool SetTimeout(int fd, int ms) {
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

IppStatus Perform(const char* host, int port, const char* resource,
                  const std::vector<uint8_t>& body, int timeout_ms) {
  IppStatus st;
  char portstr[16];
  std::snprintf(portstr, sizeof portstr, "%d", port);

  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
    st.result = IppResult::kConnectFailed; st.detail = "resolve failed"; return st;
  }
  int fd = -1;
  for (struct addrinfo* a = res; a; a = a->ai_next) {
    fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    SetTimeout(fd, timeout_ms);
    if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    ::close(fd); fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) { st.result = IppResult::kConnectFailed; st.detail = "connect failed"; return st; }

  char hdr[512];
  int hn = std::snprintf(hdr, sizeof hdr,
      "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/ipp\r\n"
      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
      resource, host, port, body.size());

  auto send_all = [&](const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    while (n) {
      ssize_t k = ::send(fd, q, n, 0);
      if (k <= 0) return false;
      q += k; n -= static_cast<size_t>(k);
    }
    return true;
  };
  if (!send_all(hdr, static_cast<size_t>(hn)) || !send_all(body.data(), body.size())) {
    ::close(fd); st.result = IppResult::kHttpError; st.detail = "send failed"; return st;
  }

  std::vector<uint8_t> resp;
  uint8_t buf[2048];
  for (;;) {
    ssize_t k = ::recv(fd, buf, sizeof buf, 0);
    if (k <= 0) break;
    resp.insert(resp.end(), buf, buf + k);
    if (resp.size() > (1u << 20)) break;
  }
  ::close(fd);

  if (resp.size() < 12) { st.result = IppResult::kMalformed; st.detail = "short response"; return st; }
  // HTTP status
  {
    const char* p = reinterpret_cast<const char*>(resp.data());
    if (std::strncmp(p, "HTTP/1.", 7) == 0) st.http_status = std::atoi(p + 9);
  }
  // Body starts after the blank line.
  static const uint8_t kSep[4] = {'\r','\n','\r','\n'};
  size_t off = 0; bool found = false;
  for (size_t i = 0; i + 4 <= resp.size(); ++i)
    if (!std::memcmp(resp.data() + i, kSep, 4)) { off = i + 4; found = true; break; }
  if (!found) { st.result = IppResult::kMalformed; st.detail = "no header terminator"; return st; }

  // Chunked bodies begin with a hex length line; skip it if present.
  if (off + 2 < resp.size() && std::isxdigit(resp[off])) {
    size_t j = off;
    while (j + 1 < resp.size() && !(resp[j] == '\r' && resp[j + 1] == '\n')) ++j;
    if (j + 2 < resp.size() && j - off <= 8) off = j + 2;
  }
  if (off + 4 > resp.size()) { st.result = IppResult::kMalformed; st.detail = "no ipp body"; return st; }

  st.ipp_status = static_cast<uint16_t>((resp[off + 2] << 8) | resp[off + 3]);
  if (st.http_status && st.http_status != 200) {
    st.result = IppResult::kHttpError;
  } else if (st.ipp_status >= 0x0400) {
    st.result = IppResult::kIppError;
  }
  st.detail = IppStatusName(st.ipp_status);
  return st;
}

IppStatus Op(uint16_t op, const char* host, int port, const char* resource,
             const char* user, int job_id, int timeout_ms) {
  char uri[256];
  std::snprintf(uri, sizeof uri, "ipp://%s:%d%s", host, port, resource);
  return Perform(host, port, resource, BuildRequest(op, uri, user, job_id), timeout_ms);
}

}  // namespace

IppStatus CancelCurrentJob(const char* h, int p, const char* r, const char* u, int t) {
  return Op(kOpCancelCurrentJob, h, p, r, u, -1, t);
}
IppStatus CancelJob(const char* h, int p, const char* r, const char* u, int id, int t) {
  return Op(kOpCancelJob, h, p, r, u, id, t);
}
IppStatus PurgeJobs(const char* h, int p, const char* r, const char* u, int t) {
  return Op(kOpPurgeJobs, h, p, r, u, -1, t);
}

const char* IppStatusName(uint16_t c) {
  switch (c) {
    case 0x0000: return "successful-ok";
    case 0x0001: return "successful-ok-ignored-or-substituted-attributes";
    case 0x0400: return "client-error-bad-request";
    case 0x0401: return "client-error-forbidden";
    case 0x0402: return "client-error-not-authenticated";
    case 0x0403: return "client-error-not-authorized";
    case 0x0405: return "client-error-not-possible";
    case 0x0406: return "client-error-timeout";
    case 0x0407: return "client-error-not-found";
    case 0x0408: return "client-error-gone";
    case 0x040C: return "client-error-attributes-or-values-not-supported";
    case 0x0501: return "server-error-operation-not-supported";
    case 0x0506: return "server-error-version-not-supported";
    default: return "unknown";
  }
}

}  // namespace brhbp
