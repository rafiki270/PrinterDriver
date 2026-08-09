#!/usr/bin/env sh
# M13b. What can be checked about the Android wrapper on a machine with no JVM.
#
# wrappers/android/README.md is blunt about the situation: there is no javac, no kotlinc,
# no Gradle and no Android SDK/NDK here, so the Kotlin sources have never been compiled.
# Two things can still be verified, and this script runs both rather than leaving the
# wrapper with no signal at all:
#
#   1. The JNI glue's C++ syntax and types, against the test-only stub for <jni.h>. This
#      is a real, adversarial check: a wrong JNI signature, a missing pd_* symbol or a
#      type mismatch fails here.
#   2. That every `external fun` declared in NativeBridge.kt has a matching
#      Java_com_printerdriver_internal_NativeBridge_<name> definition in the glue — the
#      exact mismatch that becomes an UnsatisfiedLinkError at runtime, and the one thing
#      a C++ compiler cannot notice on its own.
#
# When kotlinc happens to be installed, the Kotlin sources are parsed too.
#
# Run from the repository root; exits non-zero on the first problem.

set -e

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

echo "== JNI glue: C++ syntax and types (against src/test/cpp/jni_stub.h)"
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
  -DPD_JNI_TEST_STUB \
  -include wrappers/android/src/test/cpp/jni_stub.h \
  -I core/include -I capi/include -I capi/src -I queue/include \
  wrappers/android/src/main/cpp/printerdriver_jni.cpp
echo "   ok"

echo "== NativeBridge.kt externals all have a native definition"
bridge=wrappers/android/src/main/kotlin/com/printerdriver/internal/NativeBridge.kt
glue=wrappers/android/src/main/cpp/printerdriver_jni.cpp
missing=0
for name in $(sed -n 's/.*external fun \([A-Za-z0-9_]*\).*/\1/p' "$bridge"); do
  if ! grep -q "Java_com_printerdriver_internal_NativeBridge_${name}(" "$glue"; then
    echo "   MISSING: $name has no Java_com_printerdriver_internal_NativeBridge_${name}"
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  exit 1
fi
echo "   ok"

if command -v kotlinc >/dev/null 2>&1; then
  echo "== kotlinc: parsing the wrapper sources"
  kotlinc -nowarn -d /tmp/pd-kotlin-syntax \
    wrappers/android/src/main/kotlin/com/printerdriver/*.kt \
    wrappers/android/src/main/kotlin/com/printerdriver/internal/*.kt
  echo "   ok"
else
  echo "== kotlinc not installed: the Kotlin sources are unparsed here."
  echo "   This is the honest state of the Android wrapper (wrappers/android/README.md,"
  echo "   'Verification status'), not a passing check."
fi
