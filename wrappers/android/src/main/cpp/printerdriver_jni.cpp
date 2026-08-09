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

struct JniDriverHandle {
  pd_driver* driver = nullptr;
  JavaVM* jvm = nullptr;
  jobject logCallbackGlobalRef = nullptr;

  std::mutex callbacksMutex;
  std::vector<std::unique_ptr<JobCallbackContext>> jobCallbacks;
  std::vector<std::unique_ptr<DeviceCallbackContext>> deviceCallbacks;
  std::vector<std::unique_ptr<CustomTransportContext>> transportCallbacks;

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

} // extern "C"
