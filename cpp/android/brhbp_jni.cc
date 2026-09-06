// brhbp_jni.cc -- JNI surface for the streaming encoder.
//
// Two decisions keep the hot path off the JNI boundary entirely:
//
//   1. Output goes to a file descriptor, not a Java callback. Kotlin owns the
//      socket and its lifecycle and passes the fd in; native writes to it
//      directly. Encoded bytes never cross back.
//   2. Input arrives a *band* at a time in a direct ByteBuffer, not a row at a
//      time in a byte[]. One crossing per 64 rows instead of 64, and no copy:
//      the renderer fills the same buffer native reads.
//
// The only per-job crossings are create/begin/endPage/end/destroy.
#include "../brhbp.h"

#include <jni.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>

namespace {

// Writes straight to the socket fd Kotlin handed us.
class FdSink final : public brhbp::Sink {
 public:
  explicit FdSink(int fd) : fd_(fd) {}
  bool Write(const uint8_t* d, size_t n) override {
    while (n) {
      ssize_t k = ::write(fd_, d, n);
      if (k < 0) { if (errno == EINTR) continue; return false; }
      if (k == 0) return false;
      d += k; n -= static_cast<size_t>(k);
    }
    return true;
  }
 private:
  int fd_;
};

struct Job {
  std::unique_ptr<FdSink> sink;
  std::unique_ptr<brhbp::JobEncoder> enc;
};

inline Job* AsJob(jlong h) { return reinterpret_cast<Job*>(h); }

brhbp::Paper PaperFromOrdinal(jint v) {
  switch (v) {
    case 0: return brhbp::Paper::kA4;
    case 1: return brhbp::Paper::kLetter;
    case 2: return brhbp::Paper::kLegal;
    case 3: return brhbp::Paper::kA5;
    case 4: return brhbp::Paper::kA6;
    case 5: return brhbp::Paper::kB5;
    case 6: return brhbp::Paper::kB6;
    case 7: return brhbp::Paper::kExecutive;
    case 8: return brhbp::Paper::kC5;
    case 9: return brhbp::Paper::kDL;
    default: return brhbp::Paper::kMonarch;
  }
}

}  // namespace

extern "C" {

#define JNI_FN(name) Java_dev_brhbp_HbpEncoder_##name

JNIEXPORT jlong JNICALL JNI_FN(nativeCreate)(
    JNIEnv* env, jclass, jint fd, jint paper, jint dpi, jint copies,
    jboolean duplex, jboolean tonerSave, jstring jobName) {
  auto* job = new Job;
  job->sink = std::make_unique<FdSink>(fd);

  brhbp::JobSettings js;
  js.paper      = PaperFromOrdinal(paper);
  js.dpi        = dpi;
  js.copies     = copies;
  js.duplex     = duplex ? brhbp::Duplex::kLongEdge : brhbp::Duplex::kNone;
  js.toner_save = tonerSave == JNI_TRUE;

  const char* name = jobName ? env->GetStringUTFChars(jobName, nullptr) : nullptr;
  js.job_name = name ? name : "job";
  job->enc = std::make_unique<brhbp::JobEncoder>(job->sink.get(), js);
  if (name) env->ReleaseStringUTFChars(jobName, name);

  return reinterpret_cast<jlong>(job);
}

// {width_px, rows, stride, origin_x, origin_y}
JNIEXPORT jintArray JNICALL JNI_FN(nativeGeometry)(JNIEnv* env, jclass, jlong h) {
  const brhbp::PageGeometry& g = AsJob(h)->enc->geometry();
  jint v[5] = { g.width_px, g.rows, static_cast<jint>(g.stride), g.origin_x, g.origin_y };
  jintArray a = env->NewIntArray(5);
  env->SetIntArrayRegion(a, 0, 5, v);
  return a;
}

JNIEXPORT jboolean JNICALL JNI_FN(nativeBegin)(JNIEnv*, jclass, jlong h) {
  return AsJob(h)->enc->Begin() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL JNI_FN(nativeBeginPage)(JNIEnv*, jclass, jlong h) {
  return AsJob(h)->enc->BeginPage() ? JNI_TRUE : JNI_FALSE;
}

// buf must be a direct ByteBuffer holding nRows * stride bytes.
JNIEXPORT jboolean JNICALL JNI_FN(nativeWriteBand)(
    JNIEnv* env, jclass, jlong h, jobject buf, jint nRows) {
  auto* job = AsJob(h);
  auto* p = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buf));
  if (!p) return JNI_FALSE;                       // not a direct buffer
  const size_t stride = job->enc->geometry().stride;
  const jlong cap = env->GetDirectBufferCapacity(buf);
  if (cap < static_cast<jlong>(stride) * nRows) return JNI_FALSE;
  for (jint i = 0; i < nRows; ++i)
    if (!job->enc->WriteRow(p + static_cast<size_t>(i) * stride)) return JNI_FALSE;
  return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL JNI_FN(nativeEndPage)(JNIEnv*, jclass, jlong h) {
  return AsJob(h)->enc->EndPage() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL JNI_FN(nativeEnd)(JNIEnv*, jclass, jlong h) {
  return AsJob(h)->enc->End() ? JNI_TRUE : JNI_FALSE;
}

// Safe from any thread; only sets a flag. The writing thread then sees its
// next call fail and calls nativeAbort().
JNIEXPORT void JNICALL JNI_FN(nativeRequestCancel)(JNIEnv*, jclass, jlong h) {
  AsJob(h)->enc->RequestCancel();
}
JNIEXPORT jboolean JNICALL JNI_FN(nativeAbort)(JNIEnv*, jclass, jlong h) {
  return AsJob(h)->enc->Abort() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL JNI_FN(nativeStatus)(JNIEnv*, jclass, jlong h) {
  return static_cast<jint>(AsJob(h)->enc->status());
}
JNIEXPORT void JNICALL JNI_FN(nativeDestroy)(JNIEnv*, jclass, jlong h) {
  delete AsJob(h);
}

#undef JNI_FN
}  // extern "C"
