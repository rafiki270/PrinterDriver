// JNI glue for com.printerdriver.internal.NativeBridge (see that Kotlin file for the
// full external-fun surface this implements). Binds ONLY capi/include/printerdriver/pd.h
// -- the C ABI docs/api.md §9 calls "the whole surface" -- never the C++ headers under
// core/include or capi/src/pd_internal.hpp. That restriction is deliberate, not an
// oversight: pd.h's own header comment says wrappers are "generated-thin ... enum
// bridging plus async adapters, no logic", and binding the C++ types directly would
// both violate that (this file would then depend on ABI details pd.h intentionally
// hides behind opaque handles) and duplicate the exact hazard pd_capi.cpp's
// static_asserts exist to catch at the C++/C boundary, one layer further from where
// that guarantee is actually enforced.
//
// -- Threading contract (see also wrappers/android/README.md) -----------------------
//
// pd.h documents that job/device event callbacks run on the core's own threads: the
// synchronous replay half of pd_subscribe_job runs on whatever thread calls it (here,
// always a JNI-attached thread already, since that call itself is a JNI call), but the
// live half streams on a printer's worker thread, which is a plain native pthread the
// JVM has never seen. Every callback this file registers with the core (JobEventTrampoline,
// DeviceEventTrampoline, LogTrampoline) may therefore run on a thread with no JNIEnv,
// so each one calls AttachOrGetEnv() first, which:
//   1. asks the JavaVM for the calling thread's existing JNIEnv (GetEnv) -- covers the
//      synchronous-replay case, and covers a worker thread this file has already
//      attached and left attached (it never explicitly detaches a thread that was
//      already attached when a callback first ran on it -- see below);
//   2. falls back to AttachCurrentThreadAsDaemon() if genuinely detached.
// A thread this file attaches is detached again immediately after the callback
// returns, in the same trampoline call. This is correctness-first, not
// throughput-optimized: a core worker thread that fires many events pays one
// attach+detach pair per event rather than attaching once for the thread's lifetime.
// Given a job's event count is small (Queued..DoneSoftware is 7 events, pd.h) this is
// an acceptable default; if profiling ever shows otherwise, the fix is a
// pthread_key_create destructor that detaches on thread exit instead of after every
// callback, not a change to what gets attached.
//
// pd.h also documents that "a callback must not block, and must not call back into
// any pd_* function on the same driver." Every trampoline in this file honors that:
// each one only marshals data and calls into the JVM (CallVoidMethod on the Kotlin
// callback), never a pd_* function. The Kotlin side mirrors this -- see PrintJob.kt's
// `events` property doc for where the equivalent native-layer rule is enforced on the
// Kotlin side of a different call (checking pd_job_is_terminal from a Flow collector's
// own coroutine, never from inside the callback that feeds it).
//
// The custom-transport vtable (pd_add_printer_custom, docs/compatibility-brief.md §25)
// runs the same way but in both directions, and the direction matters. connect/write/
// close are calls INTO the JVM on the printer's worker thread, so they use the same
// attach/detach path as the trampolines above and, like them, call no pd_* function.
// pd_transport_feed_bytes is the opposite: a call OUT of the JVM from a thread this
// wrapper created (a Kotlin transport's reader), which is the only place this file
// enters the ABI from a thread the core does not own. That is why JniDriverHandle
// carries a lifecycle mutex — see the comment on it.
//
// -- Handle representation -----------------------------------------------------------
//
// pd_printer* and pd_job* cross to Kotlin as `jlong` via a direct reinterpret_cast --
// pd_capi.cpp's own interning (pd::capi::internJob) already guarantees the same
// underlying job always yields the same pd_job* pointer, so nothing extra is needed on
// this side for that guarantee to hold at the Kotlin layer too (see PrintJob.kt's
// equals/hashCode). pd_driver* is different: this file needs somewhere to keep a
// JavaVM* and the registry of GlobalRefs each subscription creates, and pd_internal.hpp
// (where the real pd_driver struct is defined) is deliberately not included here (see
// above) -- so JniDriverHandle wraps a real pd_driver* plus that bookkeeping, and IT is
// what crosses to Kotlin as the driver `jlong`, not the bare pd_driver*.

#ifndef PD_JNI_TEST_STUB
#include <jni.h>
#include <android/log.h>
#else
// See src/test/cpp/jni_stub.h's own header comment: host-side syntax checking only,
// force-included in place of the two headers above via `-DPD_JNI_TEST_STUB -include`.
// The real Android/Gradle/CMake build never defines PD_JNI_TEST_STUB, so it always
// takes the #ifndef branch; this #else changes nothing about the shipped binary.
#endif

#include "printerdriver/pd.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char* kLogTag = "PrinterDriverJNI";

// --- String marshaling ---------------------------------------------------------------

// Never returns without a valid std::string; a null jstring maps to "", matching
// pd.h's own "never NULL" convention wherever this wrapper reuses it as a C string.
std::string JStringToStd(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return std::string();
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return std::string(); // OOM inside the JVM; vanishingly unlikely, not fatal here.
  }
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

jstring StdToJString(JNIEnv* env, const char* value) {
  return env->NewStringUTF(value != nullptr ? value : "");
}

// --- Driver-scoped bookkeeping ---------------------------------------------------------
//
// One of these per pd_driver, created in driverCreate and freed in driverDestroy. It
// exists because pd_internal.hpp's real pd_driver struct is off limits here (see the
// file header comment) but this layer still needs a stable place to hang a JavaVM*
// pointer and the GlobalRefs each subscribeJob/subscribeDevice/log callback creates --
// none of which pd.h has any way to free individually, since it defines no
// pd_unsubscribe_* function at all. They are all released together, once, in
// driverDestroy, after pd_destroy(driver) has already stopped every worker thread (so
// nothing can still be using them).

struct JobCallbackContext {
  JavaVM* jvm = nullptr;
  jobject callbackGlobalRef = nullptr;
};

struct DeviceCallbackContext {
  JavaVM* jvm = nullptr;
  jobject callbackGlobalRef = nullptr;
};

// One per pd_add_printer_custom registration; `this` is the `ctx` the core hands back to
// every vtable call, so it must outlive the driver (pd.h: "`ctx` must remain alive until
// pd_destroy"). Unlike the event trampolines, the method IDs are resolved once here
// rather than per call: connect/write/close run on the printer's worker thread, and
// `write` in particular sits on the byte path of every receipt, where a
// GetObjectClass + GetMethodID pair per chunk would be pure overhead. Holding a global
// ref to the class is what makes the cached jmethodIDs safe -- they stay valid only while
// the class is not unloaded.
struct CustomTransportContext {
  JavaVM* jvm = nullptr;
  jobject callbackGlobalRef = nullptr;
  jclass callbackClassGlobalRef = nullptr;
  jmethodID connectMethod = nullptr;
  jmethodID writeMethod = nullptr;
  jmethodID closeMethod = nullptr;
};

// M16 (docs/api.md §16). One per pd_register_* call, holding the Kotlin object the core
// will call back into and the method IDs resolved once at registration -- the same
// arrangement as CustomTransportContext, and for the same reason: these run on core
// threads, some of them on the byte path of every receipt. pd.h has no unregister call,
// so like every other context here they live until driverDestroy.
struct RegistrationContext {
  JavaVM* jvm = nullptr;
  jobject callbackGlobalRef = nullptr;
  jclass callbackClassGlobalRef = nullptr;
  jmethodID methodA = nullptr;
  jmethodID methodB = nullptr;
  jmethodID methodC = nullptr;
};

struct JniDriverHandle {
  pd_driver* driver = nullptr;
  JavaVM* jvm = nullptr;
  jobject logCallbackGlobalRef = nullptr;

  std::mutex callbacksMutex;
  std::vector<std::unique_ptr<JobCallbackContext>> jobCallbacks;
  std::vector<std::unique_ptr<DeviceCallbackContext>> deviceCallbacks;
  std::vector<std::unique_ptr<CustomTransportContext>> transportCallbacks;
  std::vector<std::unique_ptr<RegistrationContext>> registrationCallbacks;

  // Guards pd_transport_feed_bytes / pd_transport_link_dropped against pd_destroy.
  // Those two are the only pd_* entry points this wrapper calls from a thread it does
  // not control -- a custom transport's reader thread -- and pd_destroy frees every
  // pd_printer handle, so a reader still running when the app calls close() would
  // otherwise dereference freed memory. driverDestroy takes this lock only to set
  // `destroyed`, then drops it before pd_destroy: a feed already inside the ABI finishes
  // first, and no new one can start.
  std::mutex lifecycleMutex;
  bool destroyed = false;
};

JniDriverHandle* AsDriverHandle(jlong handle) {
  return reinterpret_cast<JniDriverHandle*>(handle);
}

pd_printer* AsPrinter(jlong handle) {
  return reinterpret_cast<pd_printer*>(handle);
}

pd_job* AsJob(jlong handle) {
  return reinterpret_cast<pd_job*>(handle);
}

// --- Cross-thread callback attachment (see file header "Threading contract") -------

JNIEnv* AttachOrGetEnv(JavaVM* jvm, bool* didAttach) {
  *didAttach = false;
  if (jvm == nullptr) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  const jint getEnvResult = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (getEnvResult == JNI_OK) {
    return env; // Already attached -- the synchronous-replay case, or a thread this
                // file attached for an earlier callback and left attached.
  }
  if (getEnvResult != JNI_EDETACHED) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                         "JavaVM::GetEnv returned an unexpected code (%d); dropping a "
                         "callback",
                         static_cast<int>(getEnvResult));
    return nullptr;
  }
  if (jvm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                         "AttachCurrentThreadAsDaemon failed; dropping a callback");
    return nullptr;
  }
  *didAttach = true;
  return env;
}

// --- Trampolines: pd_job_event_cb / pd_device_event_cb / pd_log_cb -----------------
//
// Each of these is a plain C function pointer registered with the core (pd_subscribe_job
// / pd_subscribe_device / pd_config.log); `ctx` is always the matching *CallbackContext
// this file allocated in subscribeJob/subscribeDevice/driverCreate. None of them call
// back into any pd_* function -- see the file header "Threading contract".

// The event arrives by value (pd.h), so this trampoline owns its copy: nothing here
// depends on the emitting worker's frame still existing.
void JobEventTrampoline(pd_job* /*job*/, pd_job_event event, void* ctx) {
  auto* callbackCtx = static_cast<JobCallbackContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(callbackCtx->jvm, &didAttach);
  if (env == nullptr) {
    return;
  }

  jclass callbackClass = env->GetObjectClass(callbackCtx->callbackGlobalRef);
  // NativeJobEventCallback.onEvent(state: Int, confidence: Int, hasReason: Boolean,
  // reason: Int, monotonicMs: Long): Unit -> JNI signature "(IIZIJ)V".
  jmethodID onEvent = env->GetMethodID(callbackClass, "onEvent", "(IIZIJ)V");
  if (onEvent != nullptr) {
    env->CallVoidMethod(callbackCtx->callbackGlobalRef, onEvent,
                        static_cast<jint>(event.state), static_cast<jint>(event.confidence),
                        static_cast<jboolean>(event.has_reason != 0 ? JNI_TRUE : JNI_FALSE),
                        static_cast<jint>(event.reason),
                        static_cast<jlong>(event.monotonic_ms));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  } else {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                         "NativeJobEventCallback.onEvent not found -- check "
                         "consumer-rules.pro keep rules if R8 is enabled");
  }
  env->DeleteLocalRef(callbackClass);

  if (didAttach) {
    callbackCtx->jvm->DetachCurrentThread();
  }
}

void DeviceEventTrampoline(pd_printer* /*printer*/, pd_device_event event, void* ctx) {
  auto* callbackCtx = static_cast<DeviceCallbackContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(callbackCtx->jvm, &didAttach);
  if (env == nullptr) {
    return;
  }

  jclass callbackClass = env->GetObjectClass(callbackCtx->callbackGlobalRef);
  // NativeDeviceEventCallback.onEvent(event: Int): Unit -> JNI signature "(I)V".
  jmethodID onEvent = env->GetMethodID(callbackClass, "onEvent", "(I)V");
  if (onEvent != nullptr) {
    env->CallVoidMethod(callbackCtx->callbackGlobalRef, onEvent, static_cast<jint>(event));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  } else {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                         "NativeDeviceEventCallback.onEvent not found -- check "
                         "consumer-rules.pro keep rules if R8 is enabled");
  }
  env->DeleteLocalRef(callbackClass);

  if (didAttach) {
    callbackCtx->jvm->DetachCurrentThread();
  }
}

void LogTrampoline(const char* message, void* ctx) {
  auto* handle = static_cast<JniDriverHandle*>(ctx);
  if (handle->logCallbackGlobalRef == nullptr) {
    return;
  }
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(handle->jvm, &didAttach);
  if (env == nullptr) {
    return;
  }

  jclass callbackClass = env->GetObjectClass(handle->logCallbackGlobalRef);
  // NativeLogCallback.onMessage(message: String): Unit -> JNI signature "(Ljava/lang/String;)V".
  jmethodID onMessage = env->GetMethodID(callbackClass, "onMessage", "(Ljava/lang/String;)V");
  if (onMessage != nullptr) {
    jstring jMessage = StdToJString(env, message);
    env->CallVoidMethod(handle->logCallbackGlobalRef, onMessage, jMessage);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    env->DeleteLocalRef(jMessage);
  }
  env->DeleteLocalRef(callbackClass);

  if (didAttach) {
    handle->jvm->DetachCurrentThread();
  }
}

// --- Trampolines: pd_transport_vtable (docs/compatibility-brief.md §25) ---------------
//
// The platform owns the socket, the core owns the protocol. These three run on the
// printer's worker thread, one at a time, never concurrently with each other -- pd.h's
// thread contract above pd_transport_vtable -- so they need the same attach/detach
// treatment as the event trampolines and, like those, never call back into any pd_*
// function. Received bytes travel the other way, through
// Java_..._transportFeedBytes below, on the Kotlin transport's own reader thread.
//
// A dropped callback is reported as a transport failure rather than silently swallowed:
// returning "connected" or "wrote everything" because the JVM was unreachable would
// manufacture exactly the unearned confidence the whole SDK exists to refuse.

int32_t TransportConnectTrampoline(void* ctx) {
  auto* transportCtx = static_cast<CustomTransportContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(transportCtx->jvm, &didAttach);
  if (env == nullptr) {
    return 0;
  }

  jboolean connected = JNI_FALSE;
  if (transportCtx->connectMethod != nullptr) {
    connected = env->CallBooleanMethod(transportCtx->callbackGlobalRef,
                                       transportCtx->connectMethod);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      connected = JNI_FALSE; // An exception left the return value undefined.
    }
  }

  if (didAttach) {
    transportCtx->jvm->DetachCurrentThread();
  }
  return connected != JNI_FALSE ? 1 : 0;
}

int64_t TransportWriteTrampoline(void* ctx, const uint8_t* data, size_t size) {
  auto* transportCtx = static_cast<CustomTransportContext*>(ctx);
  if (data == nullptr || size == 0) {
    return 0;
  }
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(transportCtx->jvm, &didAttach);
  if (env == nullptr) {
    return -1;
  }

  int64_t written = -1;
  jbyteArray buffer = env->NewByteArray(static_cast<jsize>(size));
  if (buffer != nullptr && transportCtx->writeMethod != nullptr) {
    // A fresh copy per write rather than a reused direct ByteBuffer: the Kotlin side is
    // free to hand this array to a blocking OutputStream, and the core's `data` pointer
    // is only guaranteed for the duration of this call.
    env->SetByteArrayRegion(buffer, 0, static_cast<jsize>(size),
                            reinterpret_cast<const jbyte*>(data));
    const jint result = env->CallIntMethod(transportCtx->callbackGlobalRef,
                                           transportCtx->writeMethod, buffer);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      written = -1;
    } else {
      written = static_cast<int64_t>(result);
    }
  }
  if (buffer != nullptr) {
    env->DeleteLocalRef(buffer);
  }

  if (didAttach) {
    transportCtx->jvm->DetachCurrentThread();
  }
  return written;
}

void TransportCloseTrampoline(void* ctx) {
  auto* transportCtx = static_cast<CustomTransportContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(transportCtx->jvm, &didAttach);
  if (env == nullptr) {
    return;
  }

  if (transportCtx->closeMethod != nullptr) {
    env->CallVoidMethod(transportCtx->callbackGlobalRef, transportCtx->closeMethod);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }

  if (didAttach) {
    transportCtx->jvm->DetachCurrentThread();
  }
}

// --- pd_device_status packing --------------------------------------------------------
//
// Packed as 9 ints, in exactly this field order, matching DeviceStatus.fromRaw on the
// Kotlin side: [connected, observed, online, coverOpen, paperOut, paperNearEnd,
// cutterError, unrecoverableError, recoverableError]. connected/observed are plain
// 0/1; the rest are genuine tri-states (PD_UNKNOWN/-1, PD_FALSE/0, PD_TRUE/1) --
// passed through unchanged, decoded on the Kotlin side (DeviceStatus.kt).

jintArray PackDeviceStatus(JNIEnv* env, const pd_device_status& status) {
  const jint values[9] = {
      static_cast<jint>(status.connected),          static_cast<jint>(status.observed),
      static_cast<jint>(status.online),              static_cast<jint>(status.cover_open),
      static_cast<jint>(status.paper_out),           static_cast<jint>(status.paper_near_end),
      static_cast<jint>(status.cutter_error),        static_cast<jint>(status.unrecoverable_error),
      static_cast<jint>(status.recoverable_error)};
  jintArray result = env->NewIntArray(9);
  if (result != nullptr) {
    env->SetIntArrayRegion(result, 0, 9, values);
  }
  return result;
}

// --- pd_job_options assembly ----------------------------------------------------------
//
// Deliberately an out-parameter, not a return-by-value struct: `outOptions->key` is set
// to `outKeyStorage->c_str()`, i.e. it self-references sibling storage. Returning a
// struct containing both by value would only be safe if the compiler elides the
// move/copy (NRVO) -- which C++17 permits but does not guarantee for a named local
// like that -- and even where it does not apply, a short (SSO) std::string's "moved"
// copy is a fresh inline buffer at a new address, not a stolen heap pointer, so a
// pointer taken before the move can dangle after it. Writing directly into
// caller-owned locals (both declared in the same stack frame as the pd_print/
// pd_force_reprint call that uses them) sidesteps the question entirely rather than
// relying on an optimization staying in effect.
void BuildJobOptions(JNIEnv* env, jstring key, jint cut, jboolean openDrawer, jint preflight,
                     jint timeoutMs, jint topFeedDots, jint bottomFeedDots,
                     jboolean suppressVerificationId, std::string* outKeyStorage,
                     pd_job_options* outOptions) {
  *outKeyStorage = JStringToStd(env, key);
  *outOptions = pd_job_options{};
  outOptions->key = key != nullptr ? outKeyStorage->c_str() : nullptr;
  outOptions->cut = static_cast<pd_cut>(cut);
  outOptions->open_drawer = openDrawer != JNI_FALSE ? 1 : 0;
  outOptions->preflight = static_cast<pd_preflight>(preflight);
  outOptions->timeout_ms = static_cast<uint32_t>(timeoutMs);
  // Kotlin's Int is signed and a margin cannot be: a negative dot count is a caller
  // mistake, and clamping to zero prints the ticket rather than losing it.
  outOptions->top_feed_dots = topFeedDots > 0 ? static_cast<uint32_t>(topFeedDots) : 0u;
  outOptions->bottom_feed_dots =
      bottomFeedDots > 0 ? static_cast<uint32_t>(bottomFeedDots) : 0u;
  outOptions->suppress_verification_id = suppressVerificationId != JNI_FALSE ? 1 : 0;
}

// --- M16: custom method registration (docs/api.md §16) --------------------------------
//
// Every callback below is invoked BY THE CORE, on a printer's worker thread, on the
// transport reader path or on whatever thread renders a document -- so each one attaches
// exactly like the transport vtable does and, like it, calls no pd_* function. Where a
// callback cannot be served (the JVM is unreachable, the Kotlin side threw) the answer is
// the registration's own "I could not": a fence that does not fit, a matcher that does not
// recognise the bytes, a formatter that declines. None of them invents a completion.

// Copies a jbyteArray into `out`, returning what pd.h wants: the byte count, or a value
// larger than `cap` when the answer does not fit, which the core treats as the
// registration's error rather than truncating it.
size_t CopyByteArray(JNIEnv* env, jbyteArray array, uint8_t* out, size_t cap) {
  if (array == nullptr || out == nullptr) {
    return 0;
  }
  const jsize length = env->GetArrayLength(array);
  if (static_cast<size_t>(length) > cap) {
    return cap + 1;
  }
  env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(out));
  return static_cast<size_t>(length);
}

jbyteArray NewByteArray(JNIEnv* env, const uint8_t* data, size_t size) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
  if (array != nullptr && data != nullptr && size > 0) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                            reinterpret_cast<const jbyte*>(data));
  }
  return array;
}

size_t CompletionFenceTrampoline(void* ctx, const char* job_token, uint8_t* out,
                                 size_t cap) {
  auto* registration = static_cast<RegistrationContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr || out == nullptr) {
    return cap + 1; // Cannot produce a fence: the job ends Unknown, never Done.
  }

  size_t written = cap + 1;
  jstring token = StdToJString(env, job_token);
  if (registration->methodA != nullptr) {
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(
        registration->callbackGlobalRef, registration->methodA, token));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else {
      written = CopyByteArray(env, bytes, out, cap);
    }
    if (bytes != nullptr) {
      env->DeleteLocalRef(bytes);
    }
  }
  if (token != nullptr) {
    env->DeleteLocalRef(token);
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return written;
}

pd_match_result CompletionMatcherTrampoline(void* ctx, const uint8_t* data, size_t size) {
  pd_match_result result{};
  result.kind = PD_MATCH_NOT_MINE;
  auto* registration = static_cast<RegistrationContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr) {
    return result;
  }

  jbyteArray buffer = NewByteArray(env, data, size);
  if (buffer != nullptr && registration->methodB != nullptr) {
    // The verdict crosses as one nullable String so that a matcher answers in a single
    // call and nothing has to be remembered between two: null is NotMine, "" is NeedMore,
    // anything else is the matched token. See NativeCompletionMethodCallback.
    auto verdict = static_cast<jstring>(env->CallObjectMethod(
        registration->callbackGlobalRef, registration->methodB, buffer));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else if (verdict == nullptr) {
      result.kind = PD_MATCH_NOT_MINE;
    } else {
      const std::string token = JStringToStd(env, verdict);
      if (token.empty()) {
        result.kind = PD_MATCH_NEED_MORE;
      } else {
        result.kind = PD_MATCH_MATCHED;
        const size_t copied = token.size() < sizeof(result.token) - 1
                                  ? token.size()
                                  : sizeof(result.token) - 1;
        for (size_t i = 0; i < copied; ++i) {
          result.token[i] = token[i];
        }
        result.token[copied] = '\0';
      }
    }
    if (verdict != nullptr) {
      env->DeleteLocalRef(verdict);
    }
  }
  if (buffer != nullptr) {
    env->DeleteLocalRef(buffer);
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return result;
}

pd_probe_finding ProbeClassifyTrampoline(void* ctx, const uint8_t* response, size_t size) {
  pd_probe_finding finding{};
  auto* registration = static_cast<RegistrationContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr) {
    return finding;
  }

  jbyteArray buffer = NewByteArray(env, response, size);
  if (buffer != nullptr && registration->methodA != nullptr) {
    // null means the device did not answer this step; any string means it did, and is
    // the label. See NativeProbeStepCallback.
    auto label = static_cast<jstring>(env->CallObjectMethod(
        registration->callbackGlobalRef, registration->methodA, buffer));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else if (label != nullptr) {
      finding.answered = 1;
      const std::string text = JStringToStd(env, label);
      const size_t copied =
          text.size() < sizeof(finding.label) - 1 ? text.size() : sizeof(finding.label) - 1;
      for (size_t i = 0; i < copied; ++i) {
        finding.label[i] = text[i];
      }
      finding.label[copied] = '\0';
    }
    if (label != nullptr) {
      env->DeleteLocalRef(label);
    }
  }
  if (buffer != nullptr) {
    env->DeleteLocalRef(buffer);
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return finding;
}

size_t BlockHandlerTrampoline(void* ctx, const char* block_json, const char* profile_json,
                              uint8_t* out, size_t cap, int32_t* ok, char* detail,
                              size_t detail_cap) {
  auto* registration = static_cast<RegistrationContext*>(ctx);
  if (ok == nullptr || out == nullptr) {
    return 0;
  }
  *ok = 0;
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr) {
    return 0;
  }

  size_t written = 0;
  jstring block = StdToJString(env, block_json);
  jstring profile = StdToJString(env, profile_json);
  if (registration->methodA != nullptr) {
    // One tagged answer rather than two calls, so a concurrent render cannot pick up the
    // other one's reason: first byte 1 -> ESC/POS ops follow; first byte 0 -> a UTF-8
    // degradation reason follows. See NativeBlockHandlerCallback.
    auto answer = static_cast<jbyteArray>(env->CallObjectMethod(
        registration->callbackGlobalRef, registration->methodA, block, profile));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else if (answer != nullptr) {
      const jsize length = env->GetArrayLength(answer);
      if (length > 0) {
        std::vector<jbyte> bytes(static_cast<size_t>(length));
        env->GetByteArrayRegion(answer, 0, length, bytes.data());
        const size_t payload = static_cast<size_t>(length) - 1;
        if (bytes[0] != 0) {
          if (payload > cap) {
            written = cap + 1; // Never truncated: half a block is not a receipt.
          } else {
            *ok = 1;
            for (size_t i = 0; i < payload; ++i) {
              out[i] = static_cast<uint8_t>(bytes[i + 1]);
            }
            written = payload;
          }
        } else if (detail != nullptr && detail_cap > 0) {
          const size_t copied = payload < detail_cap - 1 ? payload : detail_cap - 1;
          for (size_t i = 0; i < copied; ++i) {
            detail[i] = static_cast<char>(bytes[i + 1]);
          }
          detail[copied] = '\0';
        }
      }
    }
    if (answer != nullptr) {
      env->DeleteLocalRef(answer);
    }
  }
  if (block != nullptr) {
    env->DeleteLocalRef(block);
  }
  if (profile != nullptr) {
    env->DeleteLocalRef(profile);
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return written;
}

size_t FormatterTrampoline(void* ctx, const char* value, const char* args,
                           const char* locale, char* out, size_t cap, int32_t* handled) {
  auto* registration = static_cast<RegistrationContext*>(ctx);
  if (handled == nullptr || out == nullptr) {
    return 0;
  }
  *handled = 0;
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr) {
    return 0; // Declines, so the built-in table answers instead.
  }

  size_t written = 0;
  jstring jvalue = StdToJString(env, value);
  jstring jargs = StdToJString(env, args);
  jstring jlocale = StdToJString(env, locale);
  if (registration->methodA != nullptr) {
    auto text = static_cast<jstring>(
        env->CallObjectMethod(registration->callbackGlobalRef, registration->methodA,
                              jvalue, jargs, jlocale));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else if (text != nullptr) {
      const std::string formatted = JStringToStd(env, text);
      if (formatted.size() > cap) {
        written = cap + 1;
      } else {
        *handled = 1;
        for (size_t i = 0; i < formatted.size(); ++i) {
          out[i] = formatted[i];
        }
        written = formatted.size();
      }
    }
    if (text != nullptr) {
      env->DeleteLocalRef(text);
    }
  }
  if (jvalue != nullptr) {
    env->DeleteLocalRef(jvalue);
  }
  if (jargs != nullptr) {
    env->DeleteLocalRef(jargs);
  }
  if (jlocale != nullptr) {
    env->DeleteLocalRef(jlocale);
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return written;
}

size_t DrawerKickBytesTrampoline(void* ctx, uint8_t channel, uint16_t pulse_ms,
                                 uint8_t* out, size_t cap) {
  auto* registration = static_cast<RegistrationContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr || out == nullptr) {
    return 0; // No pulse bytes: nothing is written, which is the safe answer.
  }

  size_t written = 0;
  if (registration->methodA != nullptr) {
    auto bytes = static_cast<jbyteArray>(
        env->CallObjectMethod(registration->callbackGlobalRef, registration->methodA,
                              static_cast<jint>(channel), static_cast<jint>(pulse_ms)));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else {
      written = CopyByteArray(env, bytes, out, cap);
    }
    if (bytes != nullptr) {
      env->DeleteLocalRef(bytes);
    }
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return written;
}

size_t DrawerStatusRequestTrampoline(void* ctx, uint8_t* out, size_t cap) {
  auto* registration = static_cast<RegistrationContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr || out == nullptr) {
    return 0;
  }

  size_t written = 0;
  if (registration->methodB != nullptr) {
    auto bytes = static_cast<jbyteArray>(
        env->CallObjectMethod(registration->callbackGlobalRef, registration->methodB));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else {
      written = CopyByteArray(env, bytes, out, cap);
    }
    if (bytes != nullptr) {
      env->DeleteLocalRef(bytes);
    }
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return written;
}

int32_t DrawerStatusParseTrampoline(void* ctx, const uint8_t* response, size_t size) {
  auto* registration = static_cast<RegistrationContext*>(ctx);
  bool didAttach = false;
  JNIEnv* env = AttachOrGetEnv(registration->jvm, &didAttach);
  if (env == nullptr) {
    return PD_UNKNOWN;
  }

  jint level = PD_UNKNOWN;
  jbyteArray buffer = NewByteArray(env, response, size);
  if (buffer != nullptr && registration->methodC != nullptr) {
    level = env->CallIntMethod(registration->callbackGlobalRef, registration->methodC,
                               buffer);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      level = PD_UNKNOWN; // A level nobody read is unknown, never "closed".
    }
    env->DeleteLocalRef(buffer);
  }

  if (didAttach) {
    registration->jvm->DetachCurrentThread();
  }
  return static_cast<int32_t>(level);
}

// Builds the context every pd_register_* shares: a global ref to the Kotlin object, one
// to its class (which is what keeps the cached method IDs valid), and up to three method
// IDs. Returns nullptr when a method is missing, so a registration is refused here rather
// than failing on a worker thread later.
RegistrationContext* MakeRegistration(JNIEnv* env, JniDriverHandle* handle,
                                      jobject callback, const char* nameA,
                                      const char* signatureA, const char* nameB,
                                      const char* signatureB, const char* nameC,
                                      const char* signatureC) {
  auto context = std::make_unique<RegistrationContext>();
  context->jvm = handle->jvm;
  context->callbackGlobalRef = env->NewGlobalRef(callback);
  jclass localClass = env->GetObjectClass(callback);
  context->callbackClassGlobalRef = static_cast<jclass>(env->NewGlobalRef(localClass));
  bool ok = true;
  if (nameA != nullptr) {
    context->methodA = env->GetMethodID(localClass, nameA, signatureA);
    ok = ok && context->methodA != nullptr;
  }
  if (nameB != nullptr) {
    context->methodB = env->GetMethodID(localClass, nameB, signatureB);
    ok = ok && context->methodB != nullptr;
  }
  if (nameC != nullptr) {
    context->methodC = env->GetMethodID(localClass, nameC, signatureC);
    ok = ok && context->methodC != nullptr;
  }
  env->DeleteLocalRef(localClass);
  if (!ok) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                         "a registration callback method was not found -- check "
                         "consumer-rules.pro keep rules if R8 is enabled");
    env->ExceptionClear();
    env->DeleteGlobalRef(context->callbackGlobalRef);
    env->DeleteGlobalRef(context->callbackClassGlobalRef);
    return nullptr;
  }
  RegistrationContext* raw = context.get();
  std::lock_guard<std::mutex> lock(handle->callbacksMutex);
  handle->registrationCallbacks.push_back(std::move(context));
  return raw;
}

} // namespace

extern "C" {

// --- Driver --------------------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_driverCreate(
    JNIEnv* env, jclass, jstring storageDirectory, jboolean fsyncDisabled, jobject logCallback) {
  auto* handle = new JniDriverHandle();
  env->GetJavaVM(&handle->jvm);

  const std::string storage = JStringToStd(env, storageDirectory);

  pd_config config{};
  config.storage_directory = storageDirectory != nullptr ? storage.c_str() : nullptr;
  config.fsync_disabled = fsyncDisabled != JNI_FALSE ? 1 : 0;

  if (logCallback != nullptr) {
    handle->logCallbackGlobalRef = env->NewGlobalRef(logCallback);
    config.log = &LogTrampoline;
    config.log_ctx = handle;
  }

  // pd_create copies config.storage_directory before returning (pd.h "String
  // ownership"), so `storage` going out of scope right after this call is safe.
  pd_driver* driver = pd_create(&config);
  if (driver == nullptr) {
    if (handle->logCallbackGlobalRef != nullptr) {
      env->DeleteGlobalRef(handle->logCallbackGlobalRef);
    }
    delete handle;
    return 0;
  }
  handle->driver = driver;
  return reinterpret_cast<jlong>(handle);
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_driverDestroy(
    JNIEnv* env, jclass, jlong driverHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return;
  }
  // Close the door on the custom-transport reader threads before pd_destroy frees the
  // pd_printer handles they feed. Set under the lock so a feed already inside the ABI
  // completes first; released before pd_destroy, which itself blocks on those workers
  // and must not be holding a lock a reader thread is waiting for.
  {
    std::lock_guard<std::mutex> lock(handle->lifecycleMutex);
    handle->destroyed = true;
  }

  // Stops every printer worker and waits for in-flight jobs to reach a terminal state
  // (pd_destroy's documented contract) -- no more callbacks can fire once this
  // returns, so releasing every GlobalRef below is safe.
  pd_destroy(handle->driver);

  for (auto& callback : handle->jobCallbacks) {
    env->DeleteGlobalRef(callback->callbackGlobalRef);
  }
  for (auto& callback : handle->deviceCallbacks) {
    env->DeleteGlobalRef(callback->callbackGlobalRef);
  }
  for (auto& transport : handle->transportCallbacks) {
    env->DeleteGlobalRef(transport->callbackGlobalRef);
    env->DeleteGlobalRef(transport->callbackClassGlobalRef);
  }
  for (auto& registration : handle->registrationCallbacks) {
    env->DeleteGlobalRef(registration->callbackGlobalRef);
    env->DeleteGlobalRef(registration->callbackClassGlobalRef);
  }
  if (handle->logCallbackGlobalRef != nullptr) {
    env->DeleteGlobalRef(handle->logCallbackGlobalRef);
  }
  delete handle;
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_driverLastError(
    JNIEnv* env, jclass, jlong driverHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return StdToJString(env, "");
  }
  return StdToJString(env, pd_last_error(handle->driver));
}

JNIEXPORT jobjectArray JNICALL Java_com_printerdriver_internal_NativeBridge_profileIds(
    JNIEnv* env, jclass) {
  const char* const* ids = pd_profile_ids();
  jsize count = 0;
  while (ids != nullptr && ids[count] != nullptr) {
    ++count;
  }

  jclass stringClass = env->FindClass("java/lang/String");
  jobjectArray result = env->NewObjectArray(count, stringClass, nullptr);
  for (jsize i = 0; i < count; ++i) {
    jstring value = StdToJString(env, ids[i]);
    env->SetObjectArrayElement(result, i, value);
    env->DeleteLocalRef(value);
  }
  env->DeleteLocalRef(stringClass);
  return result;
}

// --- Printers ------------------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_addPrinterTcp(
    JNIEnv* env, jclass, jlong driverHandle, jstring printerId, jstring host, jint port,
    jint widthDots, jstring profileId, jint connectTimeoutMs) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return 0;
  }

  const std::string printerIdStorage = JStringToStd(env, printerId);
  const std::string hostStorage = JStringToStd(env, host);
  const std::string profileIdStorage = JStringToStd(env, profileId);

  pd_tcp_config config{};
  config.printer_id = printerId != nullptr ? printerIdStorage.c_str() : nullptr;
  config.host = hostStorage.c_str();
  config.port = static_cast<uint16_t>(port);
  config.width_dots = static_cast<uint32_t>(widthDots);
  config.profile_id = profileId != nullptr ? profileIdStorage.c_str() : nullptr;
  config.connect_timeout_ms = static_cast<uint32_t>(connectTimeoutMs);

  pd_printer* printer = pd_add_printer_tcp(handle->driver, &config);
  return printer != nullptr ? reinterpret_cast<jlong>(printer) : 0;
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_addPrinterCustom(
    JNIEnv* env, jclass, jlong driverHandle, jobject callback, jstring description,
    jstring profileId, jint widthDots) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || callback == nullptr) {
    return 0;
  }

  auto context = std::make_unique<CustomTransportContext>();
  context->jvm = handle->jvm;
  context->callbackGlobalRef = env->NewGlobalRef(callback);

  jclass localClass = env->GetObjectClass(callback);
  context->callbackClassGlobalRef = static_cast<jclass>(env->NewGlobalRef(localClass));
  // NativeTransportCallback.connect(): Boolean / write(data: ByteArray): Int / close():
  // Unit -- see that interface's KDoc for the signature derivation.
  context->connectMethod = env->GetMethodID(localClass, "connect", "()Z");
  context->writeMethod = env->GetMethodID(localClass, "write", "([B)I");
  context->closeMethod = env->GetMethodID(localClass, "close", "()V");
  env->DeleteLocalRef(localClass);

  if (context->connectMethod == nullptr || context->writeMethod == nullptr ||
      context->closeMethod == nullptr) {
    // Refuse the registration rather than register a transport that can only fail at
    // the first write: an UnsatisfiedLinkError-shaped problem surfacing as a printer
    // that mysteriously never connects is far harder to diagnose than a null handle.
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                         "NativeTransportCallback connect/write/close not found -- check "
                         "consumer-rules.pro keep rules if R8 is enabled");
    env->ExceptionClear();
    env->DeleteGlobalRef(context->callbackGlobalRef);
    env->DeleteGlobalRef(context->callbackClassGlobalRef);
    return 0;
  }

  const std::string descriptionStorage = JStringToStd(env, description);
  const std::string profileIdStorage = JStringToStd(env, profileId);

  pd_transport_vtable vtable{};
  vtable.connect = &TransportConnectTrampoline;
  vtable.write = &TransportWriteTrampoline;
  vtable.close = &TransportCloseTrampoline;
  // Copied before pd_add_printer_custom returns (pd.h), so the local string is enough.
  vtable.description = descriptionStorage.c_str();

  CustomTransportContext* rawContext = context.get();
  // Registered before the call, not after: pd_add_printer_custom starts the printer's
  // worker thread, which can invoke TransportConnectTrampoline before this function
  // returns. The context must already be reachable and fully built by then.
  {
    std::lock_guard<std::mutex> lock(handle->callbacksMutex);
    handle->transportCallbacks.push_back(std::move(context));
  }

  pd_printer* printer =
      pd_add_printer_custom(handle->driver, &vtable, rawContext,
                            profileId != nullptr ? profileIdStorage.c_str() : nullptr,
                            static_cast<uint32_t>(widthDots));
  // A failed registration leaves its context in the vector rather than freeing it: the
  // core may already have copied the pointer, and there is no pd_remove_printer to
  // undo that. It is released with every other one in driverDestroy.
  return printer != nullptr ? reinterpret_cast<jlong>(printer) : 0;
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_transportFeedBytes(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jbyteArray data,
    jint length) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr || data == nullptr || length <= 0) {
    return JNI_FALSE;
  }

  // Held across the whole ABI call -- see JniDriverHandle::lifecycleMutex. This is the
  // one pd_* call this wrapper makes from a thread the core does not own, so it is the
  // one that has to be serialised against pd_destroy.
  std::lock_guard<std::mutex> lock(handle->lifecycleMutex);
  if (handle->destroyed) {
    return JNI_FALSE;
  }

  jbyte* elements = env->GetByteArrayElements(data, nullptr);
  if (elements == nullptr) {
    return JNI_FALSE;
  }
  const jsize available = env->GetArrayLength(data);
  const jsize count = length < available ? length : available;
  const int32_t delivered = pd_transport_feed_bytes(
      printer, reinterpret_cast<const uint8_t*>(elements), static_cast<size_t>(count));
  env->ReleaseByteArrayElements(data, elements, JNI_ABORT); // read-only use.
  return delivered != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_transportLinkDropped(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jstring message) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return JNI_FALSE;
  }
  const std::string messageStorage = JStringToStd(env, message);

  std::lock_guard<std::mutex> lock(handle->lifecycleMutex);
  if (handle->destroyed) {
    return JNI_FALSE;
  }
  return pd_transport_link_dropped(printer, messageStorage.c_str()) != 0 ? JNI_TRUE
                                                                        : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_printerId(
    JNIEnv* env, jclass, jlong printerHandle) {
  return StdToJString(env, pd_printer_id(AsPrinter(printerHandle)));
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_printerWidthDots(
    JNIEnv*, jclass, jlong printerHandle) {
  return static_cast<jint>(pd_printer_width_dots(AsPrinter(printerHandle)));
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_printerCompletionMechanism(
    JNIEnv*, jclass, jlong printerHandle) {
  return static_cast<jint>(pd_printer_completion(AsPrinter(printerHandle)));
}

JNIEXPORT jintArray JNICALL Java_com_printerdriver_internal_NativeBridge_printerStatus(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  pd_device_status status{};
  if (handle != nullptr && printer != nullptr) {
    status = pd_printer_status(handle->driver, printer);
  }
  return PackDeviceStatus(env, status);
}

JNIEXPORT jintArray JNICALL Java_com_printerdriver_internal_NativeBridge_printerRefreshStatus(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jint timeoutMs) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  pd_device_status status{};
  if (handle != nullptr && printer != nullptr) {
    status = pd_printer_refresh_status(handle->driver, printer, static_cast<uint32_t>(timeoutMs));
  }
  return PackDeviceStatus(env, status);
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_openCashDrawer(
    JNIEnv*, jclass, jlong driverHandle, jlong printerHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle != nullptr && printer != nullptr) {
    pd_open_cash_drawer(handle->driver, printer);
  }
}

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------
//
// Packed as flat int arrays, exactly like pd_device_status above: the JNI boundary carries
// numbers and the Kotlin side names them (JobModels.kt). `pin_high` keeps the ABI's
// tri-state (-1 = PD_UNKNOWN) rather than being flattened, because "the switch did not
// answer" and "the switch reads low" are different facts.

JNIEXPORT jintArray JNICALL Java_com_printerdriver_internal_NativeBridge_drawerCapabilities(
    JNIEnv* env, jclass, jlong printerHandle) {
  pd_printer* printer = AsPrinter(printerHandle);
  pd_drawer_capabilities caps{};
  if (printer != nullptr) {
    caps = pd_printer_drawer_capabilities(printer);
  } else {
    // The safe answer for a handle that is not there: no port, unsupported, unclassified.
    caps.standard = PD_DRAWER_PORT_UNKNOWN;
    caps.method = PD_DRAWER_KICK_UNSUPPORTED;
    caps.status_method = PD_DRAWER_STATUS_NONE;
  }
  const jint values[18] = {
      static_cast<jint>(caps.present),
      static_cast<jint>(caps.standard),
      static_cast<jint>(caps.voltage),
      static_cast<jint>(caps.max_current_ma),
      static_cast<jint>(caps.channel_count),
      static_cast<jint>(caps.sensor_pin),
      static_cast<jint>(caps.method),
      static_cast<jint>(caps.default_pulse_ms),
      static_cast<jint>(caps.max_pulse_ms),
      static_cast<jint>(caps.cooldown_ms),
      static_cast<jint>(caps.can_kick_during_print),
      static_cast<jint>(caps.status_available),
      static_cast<jint>(caps.status_method),
      static_cast<jint>(caps.shared_between_drawers),
      static_cast<jint>(caps.shared_with_buzzer),
      static_cast<jint>(caps.electrical_provenance),
      static_cast<jint>(caps.commands_provenance),
      static_cast<jint>(caps.kickable)};
  jintArray result = env->NewIntArray(18);
  if (result != nullptr) {
    env->SetIntArrayRegion(result, 0, 18, values);
  }
  return result;
}

JNIEXPORT jintArray JNICALL Java_com_printerdriver_internal_NativeBridge_drawerOpen(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jint channel,
    jint pulseMs) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  pd_drawer_result outcome{};
  outcome.state = PD_DRAWER_UNKNOWN;
  outcome.previous_state = PD_DRAWER_UNKNOWN;
  outcome.channel = 1;
  if (handle != nullptr && printer != nullptr) {
    pd_drawer_request request{};
    request.channel = static_cast<uint8_t>(channel);
    request.pulse_ms = static_cast<uint16_t>(pulseMs);
    outcome = pd_drawer_open(handle->driver, printer, &request);
  }
  const jint values[5] = {
      static_cast<jint>(outcome.state), static_cast<jint>(outcome.previous_state),
      static_cast<jint>(outcome.channel), static_cast<jint>(outcome.pulse_ms),
      static_cast<jint>(outcome.elapsed_ms)};
  jintArray result = env->NewIntArray(5);
  if (result != nullptr) {
    env->SetIntArrayRegion(result, 0, 5, values);
  }
  return result;
}

JNIEXPORT jintArray JNICALL Java_com_printerdriver_internal_NativeBridge_drawerReadSensor(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jint timeoutMs) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  pd_drawer_reading reading{};
  reading.pin_high = PD_UNKNOWN;
  reading.needs_calibration = PD_TRUE;
  reading.state = PD_DRAWER_UNKNOWN;
  if (handle != nullptr && printer != nullptr) {
    reading = pd_drawer_read_sensor(handle->driver, printer,
                                    static_cast<uint32_t>(timeoutMs));
  }
  const jint values[5] = {
      static_cast<jint>(reading.available), static_cast<jint>(reading.answered),
      static_cast<jint>(reading.pin_high), static_cast<jint>(reading.needs_calibration),
      static_cast<jint>(reading.state)};
  jintArray result = env->NewIntArray(5);
  if (result != nullptr) {
    env->SetIntArrayRegion(result, 0, 5, values);
  }
  return result;
}

JNIEXPORT jint JNICALL
Java_com_printerdriver_internal_NativeBridge_drawerCalibratePolarity(
    JNIEnv*, jclass, jlong driverHandle, jlong printerHandle, jboolean highMeansOpen) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return 0;
  }
  return pd_drawer_calibrate_polarity(handle->driver, printer,
                                      highMeansOpen != JNI_FALSE ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_printerdriver_internal_NativeBridge_drawerPolarityCalibrated(
    JNIEnv*, jclass, jlong driverHandle, jlong printerHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return 0;
  }
  return pd_drawer_polarity_calibrated(handle->driver, printer);
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_drawerHighMeansOpen(
    JNIEnv*, jclass, jlong driverHandle, jlong printerHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return 0;
  }
  return pd_drawer_high_means_open(handle->driver, printer);
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_printerDrain(
    JNIEnv*, jclass, jlong driverHandle, jlong printerHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle != nullptr && printer != nullptr) {
    pd_printer_drain(handle->driver, printer);
  }
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_subscribeDevice(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr || callback == nullptr) {
    return;
  }

  auto context = std::make_unique<DeviceCallbackContext>();
  context->jvm = handle->jvm;
  context->callbackGlobalRef = env->NewGlobalRef(callback);
  DeviceCallbackContext* rawContext = context.get();
  {
    std::lock_guard<std::mutex> lock(handle->callbacksMutex);
    handle->deviceCallbacks.push_back(std::move(context));
  }
  pd_subscribe_device(handle->driver, printer, &DeviceEventTrampoline, rawContext);
}

// --- Jobs: submit --------------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_printRaster(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jbyteArray pixels, jint width,
    jint height, jint strideBytes, jint binarization, jint threshold, jint maxRowsPerBand,
    jstring key, jint cut, jboolean openDrawer, jint preflight, jint timeoutMs, jint topFeedDots, jint bottomFeedDots,
    jboolean suppressVerificationId) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return 0;
  }

  jbyte* pixelElems = pixels != nullptr ? env->GetByteArrayElements(pixels, nullptr) : nullptr;

  pd_raster_rgba8 raster{};
  raster.pixels = reinterpret_cast<const uint8_t*>(pixelElems);
  raster.width = static_cast<uint32_t>(width);
  raster.height = static_cast<uint32_t>(height);
  raster.stride_bytes = static_cast<uint32_t>(strideBytes);
  raster.binarization = static_cast<pd_binarization>(binarization);
  raster.threshold = static_cast<uint8_t>(threshold);
  raster.max_rows_per_band = static_cast<uint32_t>(maxRowsPerBand);

  pd_payload payload{};
  payload.kind = PD_PAYLOAD_RASTER_RGBA8;
  payload.as.raster = raster;

  std::string keyStorage;
  pd_job_options options{};
  BuildJobOptions(env, key, cut, openDrawer, preflight, timeoutMs, topFeedDots, bottomFeedDots,
                  suppressVerificationId, &keyStorage, &options);
  pd_job* job = pd_print(handle->driver, printer, &payload, &options);

  if (pixelElems != nullptr) {
    // JNI_ABORT: read-only use, nothing to copy back.
    env->ReleaseByteArrayElements(pixels, pixelElems, JNI_ABORT);
  }

  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_printDocument(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jintArray opKinds,
    jobjectArray opTexts, jintArray opValues, jint codePage, jstring key, jint cut,
    jboolean openDrawer, jint preflight, jint timeoutMs, jint topFeedDots, jint bottomFeedDots,
    jboolean suppressVerificationId) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return 0;
  }

  const jsize count = opKinds != nullptr ? env->GetArrayLength(opKinds) : 0;
  jint* kindElems = count > 0 ? env->GetIntArrayElements(opKinds, nullptr) : nullptr;
  jint* valueElems = count > 0 ? env->GetIntArrayElements(opValues, nullptr) : nullptr;

  // Two passes: materialize every op's text into a std::vector sized once up front
  // (never reallocated afterwards), THEN take c_str() pointers into it while building
  // pd_op entries. Interleaving the passes would let a later push_back-driven growth
  // of the vector move earlier strings' storage and dangle earlier pd_op::text
  // pointers -- taking c_str() before the vector is done growing is the bug this
  // avoids.
  std::vector<std::string> textStorage(static_cast<size_t>(count));
  std::vector<bool> hasText(static_cast<size_t>(count), false);
  for (jsize i = 0; i < count; ++i) {
    jobject textObj = env->GetObjectArrayElement(opTexts, i);
    if (textObj != nullptr) {
      textStorage[static_cast<size_t>(i)] = JStringToStd(env, static_cast<jstring>(textObj));
      hasText[static_cast<size_t>(i)] = true;
      env->DeleteLocalRef(textObj);
    }
  }

  std::vector<pd_op> ops(static_cast<size_t>(count));
  for (jsize i = 0; i < count; ++i) {
    const size_t index = static_cast<size_t>(i);
    pd_op op{};
    op.kind = static_cast<pd_op_kind>(kindElems[i]);
    op.text = hasText[index] ? textStorage[index].c_str() : nullptr;
    op.value = valueElems[i];
    ops[index] = op;
  }

  pd_document document{};
  document.ops = ops.empty() ? nullptr : ops.data();
  document.count = ops.size();
  document.code_page = static_cast<pd_code_page>(codePage);

  pd_payload payload{};
  payload.kind = PD_PAYLOAD_DOCUMENT;
  payload.as.document = document;

  std::string keyStorage;
  pd_job_options options{};
  BuildJobOptions(env, key, cut, openDrawer, preflight, timeoutMs, topFeedDots, bottomFeedDots,
                  suppressVerificationId, &keyStorage, &options);
  pd_job* job = pd_print(handle->driver, printer, &payload, &options);

  if (kindElems != nullptr) {
    env->ReleaseIntArrayElements(opKinds, kindElems, JNI_ABORT);
  }
  if (valueElems != nullptr) {
    env->ReleaseIntArrayElements(opValues, valueElems, JNI_ABORT);
  }

  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

// --- M13b: the print-queue addon (docs/sdk-spec.md section 12) -------------------------
//
// A thin binding and nothing more. The three rules of section 12 -- a queue is not a
// retry engine, idempotency keys flow through, no bypass -- are enforced by the addon
// behind pd.h, so a Kotlin caller gets exactly the behaviour a C++ caller gets.
// Re-deciding any of them here would create a second queue whose rules could drift.

namespace {

pd_queue* AsQueue(jlong handle) {
  return reinterpret_cast<pd_queue*>(static_cast<intptr_t>(handle));
}

}  // namespace

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_queueCreate(
    JNIEnv*, jclass, jlong driverHandle, jboolean holdWhileOffline, jint defaultTtlMs,
    jint maxDepth, jint drainOrder) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return 0;
  }
  pd_queue_policy policy{};
  policy.hold_while_offline = holdWhileOffline != JNI_FALSE ? 1 : 0;
  // Kotlin's Int is signed and neither of these can be: a negative budget is a caller
  // mistake, and clamping to the documented zero (never expire / unlimited) is the
  // reading that keeps tickets printing.
  policy.default_ttl_ms = defaultTtlMs > 0 ? static_cast<uint32_t>(defaultTtlMs) : 0u;
  policy.max_depth = maxDepth > 0 ? static_cast<uint32_t>(maxDepth) : 0u;
  policy.drain_order = static_cast<pd_drain_order>(drainOrder);
  pd_queue* queue = pd_queue_create(handle->driver, &policy);
  return queue != nullptr ? reinterpret_cast<jlong>(queue) : 0;
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_queueDestroy(
    JNIEnv*, jclass, jlong queueHandle) {
  pd_queue_destroy(AsQueue(queueHandle));
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_queueEnqueueRaw(
    JNIEnv* env, jclass, jlong queueHandle, jlong printerHandle, jbyteArray bytes, jstring key,
    jint ttlMs, jint priority, jint cut, jboolean openDrawer, jint preflight, jint timeoutMs) {
  pd_queue* queue = AsQueue(queueHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (queue == nullptr || printer == nullptr) {
    return 0;
  }

  jbyte* byteElems = bytes != nullptr ? env->GetByteArrayElements(bytes, nullptr) : nullptr;
  const jsize byteCount = bytes != nullptr ? env->GetArrayLength(bytes) : 0;

  pd_raw raw{};
  raw.bytes = reinterpret_cast<const uint8_t*>(byteElems);
  raw.size = static_cast<size_t>(byteCount);

  pd_payload payload{};
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw = raw;

  const std::string keyStorage = JStringToStd(env, key);
  pd_queue_options options{};
  options.key = key != nullptr ? keyStorage.c_str() : nullptr;
  options.ttl_ms = ttlMs > 0 ? static_cast<uint32_t>(ttlMs) : 0u;
  options.priority = priority;
  options.cut = static_cast<pd_cut>(cut);
  options.open_drawer = openDrawer != JNI_FALSE ? 1 : 0;
  options.preflight = static_cast<pd_preflight>(preflight);
  options.timeout_ms = timeoutMs > 0 ? static_cast<uint32_t>(timeoutMs) : 0u;

  pd_job* job = pd_queue_enqueue(queue, printer, &payload, &options);

  if (byteElems != nullptr) {
    env->ReleaseByteArrayElements(bytes, byteElems, JNI_ABORT);
  }
  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_queuePause(
    JNIEnv* env, jclass, jlong queueHandle, jstring printerId) {
  const std::string id = JStringToStd(env, printerId);
  pd_queue_pause(AsQueue(queueHandle), id.c_str());
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_queueResume(
    JNIEnv* env, jclass, jlong queueHandle, jstring printerId) {
  const std::string id = JStringToStd(env, printerId);
  pd_queue_resume(AsQueue(queueHandle), id.c_str());
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_queueIsPaused(
    JNIEnv* env, jclass, jlong queueHandle, jstring printerId) {
  const std::string id = JStringToStd(env, printerId);
  return pd_queue_is_paused(AsQueue(queueHandle), id.c_str()) != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_queueIsBlocked(
    JNIEnv* env, jclass, jlong queueHandle, jstring printerId) {
  const std::string id = JStringToStd(env, printerId);
  return pd_queue_is_blocked(AsQueue(queueHandle), id.c_str()) != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_queueUnblock(
    JNIEnv* env, jclass, jlong queueHandle, jstring printerId) {
  const std::string id = JStringToStd(env, printerId);
  pd_queue_unblock(AsQueue(queueHandle), id.c_str());
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_queuePending(
    JNIEnv* env, jclass, jlong queueHandle, jstring printerId) {
  const std::string id = JStringToStd(env, printerId);
  return static_cast<jlong>(
      pd_queue_pending(AsQueue(queueHandle), printerId != nullptr ? id.c_str() : nullptr));
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_queueExpiredCount(
    JNIEnv*, jclass, jlong queueHandle) {
  return static_cast<jlong>(pd_queue_expired_count(AsQueue(queueHandle)));
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_queueOverflowCount(
    JNIEnv*, jclass, jlong queueHandle) {
  return static_cast<jlong>(pd_queue_overflow_count(AsQueue(queueHandle)));
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_queueDrainedCount(
    JNIEnv*, jclass, jlong queueHandle) {
  return static_cast<jlong>(pd_queue_drained_count(AsQueue(queueHandle)));
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_queueTick(
    JNIEnv*, jclass, jlong queueHandle) {
  pd_queue_tick(AsQueue(queueHandle));
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_printRaw(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jbyteArray bytes, jstring key,
    jint cut, jboolean openDrawer, jint preflight, jint timeoutMs, jint topFeedDots, jint bottomFeedDots,
    jboolean suppressVerificationId) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return 0;
  }

  jbyte* byteElems = bytes != nullptr ? env->GetByteArrayElements(bytes, nullptr) : nullptr;
  const jsize byteCount = bytes != nullptr ? env->GetArrayLength(bytes) : 0;

  pd_raw raw{};
  raw.bytes = reinterpret_cast<const uint8_t*>(byteElems);
  raw.size = static_cast<size_t>(byteCount);

  pd_payload payload{};
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw = raw;

  std::string keyStorage;
  pd_job_options options{};
  BuildJobOptions(env, key, cut, openDrawer, preflight, timeoutMs, topFeedDots, bottomFeedDots,
                  suppressVerificationId, &keyStorage, &options);
  pd_job* job = pd_print(handle->driver, printer, &payload, &options);

  if (byteElems != nullptr) {
    env->ReleaseByteArrayElements(bytes, byteElems, JNI_ABORT);
  }

  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_forceReprint(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jstring key, jint cut,
    jboolean openDrawer, jint preflight, jint timeoutMs, jint topFeedDots, jint bottomFeedDots,
    jboolean suppressVerificationId, jboolean suppressBanner) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr || key == nullptr) {
    return 0;
  }

  std::string keyStorage;
  pd_job_options options{};
  BuildJobOptions(env, key, cut, openDrawer, preflight, timeoutMs, topFeedDots, bottomFeedDots,
                  suppressVerificationId, &keyStorage, &options);
  pd_reprint_options reprint{};
  reprint.job = options;
  reprint.suppress_banner = suppressBanner != JNI_FALSE ? 1 : 0;
  // pd_force_reprint_opts takes the key both as its own argument and inside options
  // (the core uses the standalone argument to look the job up, and copies it into the
  // returned job's options); keyStorage is already exactly that string.
  pd_job* job =
      pd_force_reprint_opts(handle->driver, printer, keyStorage.c_str(), &reprint);
  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_jobByToken(
    JNIEnv* env, jclass, jlong driverHandle, jstring token) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || token == nullptr) {
    return 0;
  }
  const std::string tokenStorage = JStringToStd(env, token);
  pd_job* job = pd_job_by_token(handle->driver, tokenStorage.c_str());
  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_instanceNonce(
    JNIEnv* env, jclass, jlong driverHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  return StdToJString(env, handle != nullptr ? pd_instance_nonce(handle->driver) : "");
}

JNIEXPORT jlong JNICALL Java_com_printerdriver_internal_NativeBridge_findJob(
    JNIEnv* env, jclass, jlong driverHandle, jstring key) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || key == nullptr) {
    return 0;
  }
  const std::string keyStorage = JStringToStd(env, key);
  pd_job* job = pd_find_job(handle->driver, keyStorage.c_str());
  return job != nullptr ? reinterpret_cast<jlong>(job) : 0;
}

// --- Jobs: accessors -------------------------------------------------------------------

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_jobId(
    JNIEnv* env, jclass, jlong jobHandle) {
  return StdToJString(env, pd_job_id(AsJob(jobHandle)));
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_jobKey(
    JNIEnv* env, jclass, jlong jobHandle) {
  return StdToJString(env, pd_job_key(AsJob(jobHandle)));
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_jobPrintToken(
    JNIEnv* env, jclass, jlong jobHandle) {
  return StdToJString(env, pd_job_print_token(AsJob(jobHandle)));
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_jobCutToken(
    JNIEnv* env, jclass, jlong jobHandle) {
  return StdToJString(env, pd_job_cut_token(AsJob(jobHandle)));
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_jobAttempt(
    JNIEnv*, jclass, jlong jobHandle) {
  return static_cast<jint>(pd_job_attempt(AsJob(jobHandle)));
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_jobCurrentState(
    JNIEnv*, jclass, jlong jobHandle) {
  return static_cast<jint>(pd_job_current_state(AsJob(jobHandle)));
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_jobConfidence(
    JNIEnv*, jclass, jlong jobHandle) {
  return static_cast<jint>(pd_job_confidence(AsJob(jobHandle)));
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_jobIsTerminal(
    JNIEnv*, jclass, jlong jobHandle) {
  return pd_job_is_terminal(AsJob(jobHandle)) != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_printerdriver_internal_NativeBridge_subscribeJob(
    JNIEnv* env, jclass, jlong driverHandle, jlong jobHandle, jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_job* job = AsJob(jobHandle);
  if (handle == nullptr || job == nullptr || callback == nullptr) {
    return;
  }

  auto context = std::make_unique<JobCallbackContext>();
  context->jvm = handle->jvm;
  context->callbackGlobalRef = env->NewGlobalRef(callback);
  JobCallbackContext* rawContext = context.get();
  {
    std::lock_guard<std::mutex> lock(handle->callbacksMutex);
    handle->jobCallbacks.push_back(std::move(context));
  }
  // Replays every recorded event synchronously on this (the calling) thread before
  // returning, then streams the rest on the printer's worker thread -- unchanged from
  // pd_subscribe_job's own documented contract. See the file header "Threading
  // contract" for why JobEventTrampoline handles both cases uniformly.
  pd_subscribe_job(handle->driver, job, &JobEventTrampoline, rawContext);
}

JNIEXPORT jintArray JNICALL Java_com_printerdriver_internal_NativeBridge_jobAwait(
    JNIEnv* env, jclass, jlong driverHandle, jlong jobHandle, jint timeoutMs) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_job* job = AsJob(jobHandle);
  if (handle == nullptr || job == nullptr) {
    return nullptr;
  }

  pd_job_result result{};
  const int32_t settled =
      pd_job_await(handle->driver, job, static_cast<uint32_t>(timeoutMs), &result);
  if (settled == 0) {
    return nullptr; // timeout; `result` was left untouched by pd_job_await.
  }

  const jint values[5] = {static_cast<jint>(result.outcome), static_cast<jint>(result.confidence),
                          static_cast<jint>(result.reason), static_cast<jint>(result.grade),
                          static_cast<jint>(result.authority)};
  jintArray packed = env->NewIntArray(5);
  if (packed != nullptr) {
    env->SetIntArrayRegion(packed, 0, 5, values);
  }
  return packed;
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_jobMethod(
    JNIEnv* env, jclass, jlong driverHandle, jlong jobHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_job* job = AsJob(jobHandle);
  if (handle == nullptr || job == nullptr) {
    return StdToJString(env, "");
  }
  // Only ever called for a job jobAwait has already settled, so this returns at once;
  // the 1 ms budget is there so a caller who got the sequence wrong waits a moment
  // rather than forever (0 is pd.h's "wait indefinitely").
  pd_job_result result{};
  if (pd_job_await(handle->driver, job, 1, &result) == 0 || result.method == nullptr) {
    return StdToJString(env, "");
  }
  return StdToJString(env, result.method);
}

// --- M15: self-test, auto-detection and LAN discovery (docs/api.md §15) ---------------
//
// Numbers and strings cross separately, the same split pd_device_status and the drawer
// facet already use: the caller supplies a jintArray the glue fills, and the strings come
// back as the return value. Nothing here decides anything -- which provenance column
// governs a mechanism, what a printless probe may claim and how a ticket is laid out are
// all core questions, answered behind pd_self_test / pd_auto_detect / pd_discover.

namespace {

// A growable String[] builder. JNI has no such thing, and every M15 entry point returns a
// variable-length list of strings.
class JStringList {
 public:
  explicit JStringList(JNIEnv* env) : env_(env) {}

  void add(const char* value) { values_.push_back(value != nullptr ? value : ""); }
  void add(const std::string& value) { values_.push_back(value); }

  jobjectArray release() {
    jclass string_class = env_->FindClass("java/lang/String");
    jobjectArray array = env_->NewObjectArray(
        static_cast<jsize>(values_.size()), string_class, nullptr);
    if (array != nullptr) {
      for (size_t i = 0; i < values_.size(); ++i) {
        jstring value = StdToJString(env_, values_[i].c_str());
        env_->SetObjectArrayElement(array, static_cast<jsize>(i), value);
        env_->DeleteLocalRef(value);
      }
    }
    env_->DeleteLocalRef(string_class);
    return array;
  }

 private:
  JNIEnv* env_;
  std::vector<std::string> values_;
};

jobjectArray EmptyStringArray(JNIEnv* env) {
  JStringList list(env);
  return list.release();
}

// Writes `count` ints into a caller-supplied jintArray, refusing quietly when it is too
// small: a wrapper that under-sized the buffer gets zeros rather than a heap overwrite.
void WriteInts(JNIEnv* env, jintArray out, const std::vector<jint>& values) {
  if (out == nullptr) {
    return;
  }
  const jsize capacity = env->GetArrayLength(out);
  if (capacity < static_cast<jsize>(values.size())) {
    return;
  }
  env->SetIntArrayRegion(out, 0, static_cast<jsize>(values.size()), values.data());
}

// The detection report's numeric half, in the order NativeBridge documents.
void AppendSummaryInts(const pd_detection_summary& summary, std::vector<jint>* out) {
  out->push_back(static_cast<jint>(summary.identity_trusted));
  out->push_back(static_cast<jint>(summary.confidence_percent));
  out->push_back(static_cast<jint>(summary.impersonation_suspected));
  out->push_back(static_cast<jint>(summary.identity_fresh));
  out->push_back(static_cast<jint>(summary.selection));
  out->push_back(static_cast<jint>(summary.nominal_paper_mm));
  out->push_back(static_cast<jint>(summary.printable_width_dots));
  out->push_back(static_cast<jint>(summary.chars_per_line));
  out->push_back(static_cast<jint>(summary.dpi));
  out->push_back(static_cast<jint>(summary.completion));
  out->push_back(static_cast<jint>(summary.grade_ceiling));
  out->push_back(static_cast<jint>(summary.authority));
  out->push_back(static_cast<jint>(summary.completion_provenance));
  out->push_back(static_cast<jint>(summary.drawer_present));
  out->push_back(static_cast<jint>(summary.drawer_kickable));
  out->push_back(static_cast<jint>(summary.drawer_standard));
  out->push_back(static_cast<jint>(summary.drawer_voltage));
}

// And its string half, minus the degradations, which the caller appends last so their
// count can be the final int.
void AppendSummaryStrings(const pd_detection_summary& summary, JStringList* out) {
  out->add(summary.endpoint);
  out->add(summary.vendor);
  out->add(summary.model);
  out->add(summary.firmware);
  out->add(summary.serial);
  out->add(summary.profile_id);
  out->add(summary.method);
  out->add(summary.provenance_summary);
}

void AppendDegradations(const pd_detection_summary& summary, JStringList* out) {
  for (size_t i = 0; i < summary.degradation_count; ++i) {
    out->add(summary.degradations[i]);
  }
}

}  // namespace

JNIEXPORT jobjectArray JNICALL Java_com_printerdriver_internal_NativeBridge_selfTest(
    JNIEnv* env, jclass, jlong driverHandle, jlong printerHandle, jstring key,
    jboolean refreshIdentity, jboolean probeWithoutPrinting, jboolean barcode,
    jstring barcodeData, jboolean printVerificationId, jint timeoutMs, jintArray values) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_printer* printer = AsPrinter(printerHandle);
  if (handle == nullptr || printer == nullptr) {
    return EmptyStringArray(env);
  }

  const std::string key_text = JStringToStd(env, key);
  const std::string barcode_text = JStringToStd(env, barcodeData);
  pd_self_test_options options{};
  options.key = key != nullptr ? key_text.c_str() : nullptr;
  options.refresh_identity = refreshIdentity ? 1 : 0;
  options.probe_without_printing = probeWithoutPrinting ? 1 : 0;
  options.no_barcode = barcode ? 0 : 1;
  options.barcode_data = barcodeData != nullptr ? barcode_text.c_str() : nullptr;
  options.no_verification_id = printVerificationId ? 0 : 1;
  options.timeout_ms = static_cast<uint32_t>(timeoutMs);

  pd_self_test_result result{};
  if (pd_self_test(handle->driver, printer, &options, &result) == 0) {
    return EmptyStringArray(env);
  }

  // The ticket arrives as one '\n'-separated block; JNI carries it as lines, so the
  // Kotlin side never has to know how it was joined.
  std::vector<std::string> ticket_lines;
  {
    const std::string ticket = result.ticket_text != nullptr ? result.ticket_text : "";
    std::string line;
    for (const char c : ticket) {
      if (c == '\n') {
        ticket_lines.push_back(line);
        line.clear();
      } else {
        line.push_back(c);
      }
    }
    if (!line.empty()) {
      ticket_lines.push_back(line);
    }
  }

  std::vector<jint> ints;
  ints.push_back(static_cast<jint>(result.result.outcome));
  ints.push_back(static_cast<jint>(result.result.confidence));
  ints.push_back(static_cast<jint>(result.result.reason));
  ints.push_back(static_cast<jint>(result.result.grade));
  ints.push_back(static_cast<jint>(result.result.authority));
  AppendSummaryInts(result.detection, &ints);
  ints.push_back(static_cast<jint>(result.detection.degradation_count));
  ints.push_back(static_cast<jint>(ticket_lines.size()));
  WriteInts(env, values, ints);

  JStringList strings(env);
  strings.add(result.result.method);
  strings.add(result.key);
  strings.add(result.print_token);
  AppendSummaryStrings(result.detection, &strings);
  AppendDegradations(result.detection, &strings);
  for (const std::string& line : ticket_lines) {
    strings.add(line);
  }
  return strings.release();
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_autoDetect(
    JNIEnv* env, jclass, jlong driverHandle, jstring subnetCidr, jobjectArray endpoints,
    jint port, jint concurrency, jint connectTimeoutMs, jint responseTimeoutMs,
    jboolean probeUnknown) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return -1;
  }

  const std::string cidr = JStringToStd(env, subnetCidr);
  // Owned here for the duration of the call: pd.h copies every string it is given before
  // returning, so these outlive it by construction.
  std::vector<std::string> endpoint_texts;
  std::vector<const char*> endpoint_pointers;
  if (endpoints != nullptr) {
    const jsize count = env->GetArrayLength(endpoints);
    endpoint_texts.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
      jobject element = env->GetObjectArrayElement(endpoints, i);
      endpoint_texts.push_back(JStringToStd(env, static_cast<jstring>(element)));
      env->DeleteLocalRef(element);
    }
    for (const std::string& text : endpoint_texts) {
      endpoint_pointers.push_back(text.c_str());
    }
    endpoint_pointers.push_back(nullptr);
  }

  pd_auto_detect_options options{};
  options.subnet_cidr = subnetCidr != nullptr ? cidr.c_str() : nullptr;
  options.endpoints = endpoint_pointers.empty() ? nullptr : endpoint_pointers.data();
  options.port = static_cast<uint16_t>(port);
  options.concurrency = static_cast<uint32_t>(concurrency);
  options.connect_timeout_ms = static_cast<uint32_t>(connectTimeoutMs);
  options.response_timeout_ms = static_cast<uint32_t>(responseTimeoutMs);
  options.leave_unknown_unprobed = probeUnknown ? 0 : 1;
  // No callback: the results are read back by index, so a JNI thread attach per candidate
  // never happens (pd.h, pd_detected_at).
  return static_cast<jint>(pd_auto_detect(handle->driver, &options, nullptr, nullptr));
}

JNIEXPORT jobjectArray JNICALL Java_com_printerdriver_internal_NativeBridge_detectedAt(
    JNIEnv* env, jclass, jlong driverHandle, jint index, jintArray values) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_detected_printer one{};
  if (handle == nullptr || pd_detected_at(handle->driver, index, &one) == 0) {
    return EmptyStringArray(env);
  }

  std::vector<jint> ints;
  ints.push_back(static_cast<jint>(one.port));
  ints.push_back(static_cast<jint>(one.status));
  ints.push_back(static_cast<jint>(one.port_open));
  ints.push_back(static_cast<jint>(one.from_cache));
  AppendSummaryInts(one.summary, &ints);
  ints.push_back(static_cast<jint>(one.summary.degradation_count));
  WriteInts(env, values, ints);

  JStringList strings(env);
  strings.add(one.endpoint);
  strings.add(one.host);
  strings.add(one.dle_eot_hex);
  AppendSummaryStrings(one.summary, &strings);
  AppendDegradations(one.summary, &strings);
  return strings.release();
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_discover(
    JNIEnv* env, jclass, jlong driverHandle, jstring subnetCidr, jint port,
    jint concurrency, jint connectTimeoutMs, jint responseTimeoutMs,
    jboolean probeBackchannel) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return -1;
  }
  const std::string cidr = JStringToStd(env, subnetCidr);
  pd_discover_options options{};
  options.subnet_cidr = subnetCidr != nullptr ? cidr.c_str() : nullptr;
  options.port = static_cast<uint16_t>(port);
  options.concurrency = static_cast<uint32_t>(concurrency);
  options.connect_timeout_ms = static_cast<uint32_t>(connectTimeoutMs);
  options.response_timeout_ms = static_cast<uint32_t>(responseTimeoutMs);
  options.no_backchannel_probe = probeBackchannel ? 0 : 1;
  return static_cast<jint>(pd_discover(handle->driver, &options, nullptr, nullptr));
}

JNIEXPORT jobjectArray JNICALL Java_com_printerdriver_internal_NativeBridge_discoveredAt(
    JNIEnv* env, jclass, jlong driverHandle, jint index, jintArray values) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  pd_discovered_device one{};
  if (handle == nullptr || pd_discovered_at(handle->driver, index, &one) == 0) {
    return EmptyStringArray(env);
  }
  const std::vector<jint> ints = {static_cast<jint>(one.port),
                                  static_cast<jint>(one.port9100_open)};
  WriteInts(env, values, ints);

  JStringList strings(env);
  strings.add(one.ip);
  strings.add(one.dle_eot_hex);
  return strings.release();
}

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_localSubnet(
    JNIEnv* env, jclass, jlong driverHandle) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr) {
    return StdToJString(env, "");
  }
  return StdToJString(env, pd_local_subnet(handle->driver));
}

// --- Printer facts the wrapper had not surfaced ---------------------------------------

JNIEXPORT jint JNICALL
Java_com_printerdriver_internal_NativeBridge_printerCompletionProvenance(
    JNIEnv*, jclass, jlong printerHandle) {
  return static_cast<jint>(pd_printer_completion_provenance(AsPrinter(printerHandle)));
}

JNIEXPORT jint JNICALL Java_com_printerdriver_internal_NativeBridge_printerLanguage(
    JNIEnv*, jclass, jlong printerHandle) {
  return static_cast<jint>(pd_printer_language(AsPrinter(printerHandle)));
}

// --- The core's own spelling of a mirrored enum ---------------------------------------
//
// One entry point rather than sixteen: the family selector below is an implementation
// detail of com.printerdriver.internal.AbiEnum, and the Kotlin surface is an `abiName`
// property on each enum. Every pd_*_name function in pd.h is reachable through it.

JNIEXPORT jstring JNICALL Java_com_printerdriver_internal_NativeBridge_abiEnumName(
    JNIEnv* env, jclass, jint family, jint value) {
  const char* name = "";
  switch (family) {
    case 0: name = pd_job_state_name(static_cast<pd_job_state>(value)); break;
    case 1: name = pd_confidence_level_name(static_cast<pd_confidence_level>(value)); break;
    case 2: name = pd_device_event_name(static_cast<pd_device_event>(value)); break;
    case 3: name = pd_failure_reason_name(static_cast<pd_failure_reason>(value)); break;
    case 4: name = pd_job_outcome_name(static_cast<pd_job_outcome>(value)); break;
    case 5: name = pd_confidence_grade_name(static_cast<pd_confidence_grade>(value)); break;
    case 6:
      name = pd_completion_authority_name(static_cast<pd_completion_authority>(value));
      break;
    case 7: name = pd_provenance_name(static_cast<pd_provenance>(value)); break;
    case 8: name = pd_command_language_name(static_cast<pd_command_language>(value)); break;
    case 9: name = pd_payload_kind_name(static_cast<pd_payload_kind>(value)); break;
    case 10:
      name = pd_completion_mechanism_name(static_cast<pd_completion_mechanism>(value));
      break;
    case 11: name = pd_cut_variant_name(static_cast<pd_cut_variant>(value)); break;
    case 12: name = pd_drawer_state_name(static_cast<pd_drawer_state>(value)); break;
    case 13:
      name = pd_drawer_port_standard_name(static_cast<pd_drawer_port_standard>(value));
      break;
    case 14:
      name = pd_drawer_kick_method_name(static_cast<pd_drawer_kick_method>(value));
      break;
    case 15:
      name = pd_drawer_status_method_name(static_cast<pd_drawer_status_method>(value));
      break;
    case 16:
      name = pd_profile_selection_name(static_cast<pd_profile_selection>(value));
      break;
    case 17: name = pd_detection_status_name(static_cast<pd_detection_status>(value)); break;
    case 18: name = pd_drain_order_name(static_cast<pd_drain_order>(value)); break;
    case 19: name = pd_match_kind_name(static_cast<pd_match_kind>(value)); break;
    case 20:
      // Not a member name: the letter a report tabulates ("A+", "A".."E").
      name = pd_confidence_grade_letter(static_cast<pd_confidence_grade>(value));
      break;
    default: name = ""; break;
  }
  return StdToJString(env, name);
}

// --- M16: custom method registration (docs/api.md §16) --------------------------------

JNIEXPORT jboolean JNICALL
Java_com_printerdriver_internal_NativeBridge_registerCompletionMethod(
    JNIEnv* env, jclass, jlong driverHandle, jstring id, jstring methodName, jint grade,
    jint authority, jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || callback == nullptr) {
    return JNI_FALSE;
  }
  RegistrationContext* context =
      MakeRegistration(env, handle, callback, "fenceBytes", "(Ljava/lang/String;)[B",
                       "match", "([B)Ljava/lang/String;", nullptr, nullptr);
  if (context == nullptr) {
    return JNI_FALSE;
  }

  const std::string idStorage = JStringToStd(env, id);
  const std::string nameStorage = JStringToStd(env, methodName);
  pd_completion_method method{};
  method.id = idStorage.c_str();
  method.fence_bytes = &CompletionFenceTrampoline;
  method.matcher = &CompletionMatcherTrampoline;
  method.ctx = context;
  method.grade = static_cast<pd_confidence_grade>(grade);
  method.authority = static_cast<pd_completion_authority>(authority);
  method.method_name = nameStorage.c_str();
  return pd_register_completion_method(handle->driver, &method) == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_registerProbeStep(
    JNIEnv* env, jclass, jlong driverHandle, jstring id, jbyteArray requestBytes,
    jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || callback == nullptr) {
    return JNI_FALSE;
  }
  RegistrationContext* context =
      MakeRegistration(env, handle, callback, "classify", "([B)Ljava/lang/String;",
                       nullptr, nullptr, nullptr, nullptr);
  if (context == nullptr) {
    return JNI_FALSE;
  }

  std::vector<uint8_t> request;
  if (requestBytes != nullptr) {
    const jsize length = env->GetArrayLength(requestBytes);
    request.resize(static_cast<size_t>(length));
    if (length > 0) {
      env->GetByteArrayRegion(requestBytes, 0, length,
                              reinterpret_cast<jbyte*>(request.data()));
    }
  }

  const std::string idStorage = JStringToStd(env, id);
  pd_probe_step step{};
  step.id = idStorage.c_str();
  step.request_bytes = request.empty() ? nullptr : request.data();
  step.request_size = request.size();
  step.classify = &ProbeClassifyTrampoline;
  step.ctx = context;
  // The core copies request_bytes before returning (pd.h), and refuses a step whose
  // bytes could print -- which is why this can be a local vector.
  return pd_register_probe_step(handle->driver, &step) == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_printerdriver_internal_NativeBridge_registerBlockHandler(
    JNIEnv* env, jclass, jlong driverHandle, jstring kind, jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || callback == nullptr) {
    return JNI_FALSE;
  }
  RegistrationContext* context = MakeRegistration(
      env, handle, callback, "render", "(Ljava/lang/String;Ljava/lang/String;)[B", nullptr,
      nullptr, nullptr, nullptr);
  if (context == nullptr) {
    return JNI_FALSE;
  }

  const std::string kindStorage = JStringToStd(env, kind);
  pd_block_handler blockHandler{};
  blockHandler.kind = kindStorage.c_str();
  blockHandler.handler = &BlockHandlerTrampoline;
  blockHandler.ctx = context;
  return pd_register_block_handler(handle->driver, &blockHandler) == 1 ? JNI_TRUE
                                                                       : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_printerdriver_internal_NativeBridge_registerFormatter(
    JNIEnv* env, jclass, jlong driverHandle, jstring name, jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || callback == nullptr) {
    return JNI_FALSE;
  }
  RegistrationContext* context = MakeRegistration(
      env, handle, callback, "format",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      nullptr, nullptr, nullptr, nullptr);
  if (context == nullptr) {
    return JNI_FALSE;
  }

  const std::string nameStorage = JStringToStd(env, name);
  pd_formatter formatter{};
  formatter.name = nameStorage.c_str();
  formatter.formatter = &FormatterTrampoline;
  formatter.ctx = context;
  return pd_register_formatter(handle->driver, &formatter) == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_printerdriver_internal_NativeBridge_registerDrawerKick(
    JNIEnv* env, jclass, jlong driverHandle, jstring id, jboolean readableSwitch,
    jobject callback) {
  JniDriverHandle* handle = AsDriverHandle(driverHandle);
  if (handle == nullptr || callback == nullptr) {
    return JNI_FALSE;
  }
  RegistrationContext* context =
      MakeRegistration(env, handle, callback, "kickBytes", "(II)[B", "statusRequest",
                       "()[B", "statusParse", "([B)I");
  if (context == nullptr) {
    return JNI_FALSE;
  }

  const std::string idStorage = JStringToStd(env, id);
  pd_drawer_kick_reg reg{};
  reg.id = idStorage.c_str();
  reg.kick_bytes = &DrawerKickBytesTrampoline;
  // Both halves or neither, as pd.h requires: no readable switch means a kick reports
  // KICK_SENT_UNVERIFIED rather than claiming a verified open.
  reg.status_request = readableSwitch ? &DrawerStatusRequestTrampoline : nullptr;
  reg.status_parse = readableSwitch ? &DrawerStatusParseTrampoline : nullptr;
  reg.ctx = context;
  return pd_register_drawer_kick(handle->driver, &reg) == 1 ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
