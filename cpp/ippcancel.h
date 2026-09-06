// ippcancel.h -- out-of-band job cancellation over IPP (port 631).
//
// The in-band path (JobEncoder::Abort) can only abandon what has not been sent
// yet. It cannot recall pages the device has already committed. IPP is the
// only channel this printer offers that can act on a job the device is already
// holding, so a client needs both:
//
//   Abort()          stop feeding, close the raster escape, end the job
//   CancelCurrent()  ask the device to drop the job it is working on
//   PurgeAll()       drop everything queued
//
// Deliberately dependency-free: IPP is an HTTP POST with a binary body, and
// the three operations here need perhaps twenty attribute bytes each.
#ifndef IPPCANCEL_H_
#define IPPCANCEL_H_

#include <cstdint>
#include <string>

namespace brhbp {

enum class IppResult {
  kOk = 0,
  kConnectFailed,
  kHttpError,
  kIppError,        // reached the printer, which refused
  kMalformed,
};

struct IppStatus {
  IppResult result = IppResult::kOk;
  uint16_t  ipp_status = 0;      // e.g. 0x0000 successful-ok
  int       http_status = 0;
  std::string detail;
};

// host: bare address, e.g. "10.0.0.5". resource: usually "/ipp/print".
IppStatus CancelCurrentJob(const char* host, int port, const char* resource,
                           const char* user, int timeout_ms = 5000);
IppStatus CancelJob(const char* host, int port, const char* resource,
                    const char* user, int job_id, int timeout_ms = 5000);
IppStatus PurgeJobs(const char* host, int port, const char* resource,
                    const char* user, int timeout_ms = 5000);

const char* IppStatusName(uint16_t code);

}  // namespace brhbp
#endif  // IPPCANCEL_H_
