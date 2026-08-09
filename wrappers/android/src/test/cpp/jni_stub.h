// Minimal, test-only stand-in for the two real Android NDK headers
// src/main/cpp/printerdriver_jni.cpp includes (<jni.h> and <android/log.h>), neither
// of which exists on a host with no Android SDK/NDK installed.
//
// This file exists SOLELY so that
//
//   clang++ -std=c++17 -fsyntax-only -DPD_JNI_TEST_STUB -include jni_stub.h ...
//
// can parse and type-check printerdriver_jni.cpp against the real pd.h on a plain
// host compiler -- see README.md "Syntax confidence without a JVM" for the exact
// command this repository's scaffold was checked with. It is a syntax/type-check
// fixture, NOT a functional JNI implementation:
//   - every JNIEnv/JavaVM method below is declared but never defined (fine for
//     -fsyntax-only, which never reaches the linker);
//   - reference types mirror the real jni.h's class hierarchy (_jobject <- _jclass,
//     _jstring, _jarray <- _jbyteArray etc.) closely enough to catch real type
//     mistakes in the glue, but the layout is not ABI-compatible with any real JVM;
//   - it is never compiled into the shipped library. The real Android/Gradle/CMake
///    build (CMakeLists.txt, invoked by externalNativeBuild) always uses the NDK's own
//    <jni.h>/<android/log.h> -- this file is not referenced anywhere in that path.
//
// Keep this in sync with whatever subset of the JNI/NDK API
// src/main/cpp/printerdriver_jni.cpp actually calls; it only needs to cover that
// subset, not the full API surface.

#pragma once

#include <cstdarg>
#include <cstdint>

// --- <jni.h> subset ------------------------------------------------------------------

using jint = int32_t;
using jlong = int64_t;
using jbyte = int8_t;
using jboolean = uint8_t;
using jsize = jint;

constexpr jboolean JNI_FALSE = 0;
constexpr jboolean JNI_TRUE = 1;
constexpr jint JNI_OK = 0;
constexpr jint JNI_ERR = -1;
constexpr jint JNI_EDETACHED = -2;
constexpr jint JNI_EVERSION = -3;
constexpr jint JNI_VERSION_1_6 = 0x00010006;
constexpr jint JNI_ABORT = 2;
constexpr jint JNI_COMMIT = 1;

#define JNIEXPORT
#define JNICALL

class _jobject {};
class _jclass : public _jobject {};
class _jstring : public _jobject {};
class _jthrowable : public _jobject {};
class _jarray : public _jobject {};
class _jbyteArray : public _jarray {};
class _jintArray : public _jarray {};
class _jlongArray : public _jarray {};
class _jobjectArray : public _jarray {};

using jobject = _jobject*;
using jclass = _jclass*;
using jstring = _jstring*;
using jthrowable = _jthrowable*;
using jarray = _jarray*;
using jbyteArray = _jbyteArray*;
using jintArray = _jintArray*;
using jlongArray = _jlongArray*;
using jobjectArray = _jobjectArray*;

struct _jmethodID;
using jmethodID = _jmethodID*;
struct _jfieldID;
using jfieldID = _jfieldID*;

class _JavaVM;
using JavaVM = _JavaVM;

// The real jni.h's C++-only convenience view: member functions instead of the C
// struct-of-function-pointers dispatch, which is what lets printerdriver_jni.cpp call
// `env->Method(...)` directly. Only the subset the glue file actually calls is
// declared.
class _JNIEnv {
 public:
  jint GetVersion();

  jclass FindClass(const char* name);

  jmethodID GetMethodID(jclass clazz, const char* name, const char* sig);

  void CallVoidMethod(jobject obj, jmethodID methodID, ...);
  jboolean CallBooleanMethod(jobject obj, jmethodID methodID, ...);
  jint CallIntMethod(jobject obj, jmethodID methodID, ...);

  jobject NewGlobalRef(jobject obj);
  void DeleteGlobalRef(jobject globalRef);
  void DeleteLocalRef(jobject localRef);

  jclass GetObjectClass(jobject obj);

  jstring NewStringUTF(const char* bytes);
  const char* GetStringUTFChars(jstring str, jboolean* isCopy);
  void ReleaseStringUTFChars(jstring str, const char* chars);

  jsize GetArrayLength(jarray array);

  jbyteArray NewByteArray(jsize len);
  jbyte* GetByteArrayElements(jbyteArray array, jboolean* isCopy);
  void ReleaseByteArrayElements(jbyteArray array, jbyte* elems, jint mode);
  void SetByteArrayRegion(jbyteArray array, jsize start, jsize len, const jbyte* buf);

  jint* GetIntArrayElements(jintArray array, jboolean* isCopy);
  void ReleaseIntArrayElements(jintArray array, jint* elems, jint mode);
  jintArray NewIntArray(jsize len);
  void SetIntArrayRegion(jintArray array, jsize start, jsize len, const jint* buf);

  jobject GetObjectArrayElement(jobjectArray array, jsize index);
  void SetObjectArrayElement(jobjectArray array, jsize index, jobject val);
  jobjectArray NewObjectArray(jsize len, jclass elementClass, jobject initialElement);

  jboolean ExceptionCheck();
  jthrowable ExceptionOccurred();
  void ExceptionDescribe();
  void ExceptionClear();
  jint ThrowNew(jclass clazz, const char* message);

  jint GetJavaVM(JavaVM** vm);
};
using JNIEnv = _JNIEnv;

class _JavaVM {
 public:
  jint AttachCurrentThread(JNIEnv** env, void* args);
  jint AttachCurrentThreadAsDaemon(JNIEnv** env, void* args);
  jint DetachCurrentThread();
  jint GetEnv(void** env, jint version);
};

// --- <android/log.h> subset -----------------------------------------------------------

constexpr int ANDROID_LOG_WARN = 5;
constexpr int ANDROID_LOG_ERROR = 6;

int __android_log_print(int prio, const char* tag, const char* fmt, ...);
