// status.h -- device status by SNMP polling.
//
// Port 9100 offers no back-channel on this firmware: @PJL USTATUS answers "?"
// like an invented variable name, and holding the socket open across two
// complete jobs returned zero bytes. SNMP is the only channel that reports
// progress, and unlike a second 9100 connection it can be used *while* a job
// prints.
//
// Three OIDs are enough for a progress UI:
//   hrPrinterStatus              idle / printing / warmup / other
//   hrPrinterDetectedErrorState  a bit field: no paper, jam, cover open, ...
//   prtMarkerLifeCount           increments once per impression
#ifndef BRHBP_STATUS_H_
#define BRHBP_STATUS_H_

#include <cstdint>
#include <string>

namespace brhbp {

enum class PrinterState { kUnknown = 0, kOther = 1, kIdle = 3, kPrinting = 4, kWarmup = 5 };

// Bits of hrPrinterDetectedErrorState (RFC 1759), byte 0 unless noted.
enum ErrorBit : uint16_t {
  kLowPaper     = 1 << 0,
  kNoPaper      = 1 << 1,
  kLowToner     = 1 << 2,
  kNoToner      = 1 << 3,
  kDoorOpen     = 1 << 4,
  kJammed       = 1 << 5,
  kOffline      = 1 << 6,
  kServiceReq   = 1 << 7,
};

struct DeviceStatus {
  bool         ok = false;           // false = no reply / malformed
  PrinterState state = PrinterState::kUnknown;
  uint16_t     errors = 0;           // ErrorBit mask
  int64_t      life_count = -1;      // impressions, -1 if unavailable
  std::string  detail;
};

// One round trip, UDP/161. timeout_ms applies per request.
DeviceStatus PollStatus(const char* host, const char* community = "public",
                        int timeout_ms = 1500);

std::string DescribeErrors(uint16_t mask);
const char* StateName(PrinterState s);

// Exposed for testing: encode an SNMPv1 GetRequest for one OID.
// oid is dotted decimal, e.g. "1.3.6.1.2.1.25.3.5.1.1.1".
bool EncodeGetRequest(const char* community, const char* oid, int32_t request_id,
                      std::string* out);

}  // namespace brhbp
#endif  // BRHBP_STATUS_H_
