#!/usr/bin/env bash
#
# Syntax- and type-check the React Native TurboModule on a host with no React Native
# application, no Xcode workspace and no Android NDK.
#
# WHAT THIS PROVES
#   Every pd_* call in cpp/PrinterDriverModule.cpp has the right name, arity and argument
#   types against the REAL capi/include/printerdriver/pd.h -- the same header the Swift
#   package, the Android AAR and the .NET wrapper bind. A misspelled function, a
#   pd_printer* passed where a pd_driver* belongs, a struct field that no longer exists, a
#   callback signature that drifted: all of those fail here.
#
#   It also proves the module type-checks against a faithful model of JSI's OWNERSHIP
#   rules. cpp/__tests__/rn_stub.h makes jsi::Value, jsi::Object and jsi::String move-only,
#   exactly as the real jsi.h does, so a stray copy is a compile error here as it would be
#   in a real build.
#
# WHAT THIS DOES NOT PROVE
#   Nothing about behaviour. Nothing about the real React Native headers, which this stub
#   models in the parts the module uses and was not compared to byte for byte. Nothing
#   about codegen, autolinking, the podspec, the Gradle build, or whether a single byte
#   reaches a printer from a React Native app. README.md "Verification status" carries the
#   ordered list of what a real app and CI still have to confirm.
#
# THE NEGATIVE CONTROLS
#   A green check is worthless if the check cannot go red, so this script also runs three
#   deliberate failures and requires each one to fail FOR ITS OWN STATED REASON:
#
#     A. pd_printer_id() called with a pd_driver* -- the canonical binding mistake, and
#        proof that pd.h is really being type-checked rather than merely parsed.
#     B. a jsi::Value copied -- proof that the stub models JSI's move-only ownership, and
#        therefore that a real ownership bug in the module would not slip through.
#     C. the module compiled WITHOUT the stub -- it must fail with
#        "'ReactCommon/CallInvoker.h' file not found", the first of the three React Native
#        headers it includes. This is the control that matters most: without it, a green
#        check could mean the React Native code path had been preprocessed away and the
#        compiler never saw the module at all. It proves the #ifndef PD_RN_TEST_STUB
#        branch is real and that the stub is what is satisfying it.
#
# Usage: bash scripts/check_rn_cpp_syntax.sh   (from anywhere; paths resolve from the
# package root, which is this script's parent directory.)

set -u

package="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo="$(cd "${package}/../.." && pwd)"
cxx="${CXX:-clang++}"

flags=(
  -std=c++17
  -fsyntax-only
  -Wall -Wextra -Wpedantic
  -DPD_RN_TEST_STUB
  -include "${package}/cpp/__tests__/rn_stub.h"
  -I "${package}/cpp"
  -I "${repo}/capi/include"
)

failures=0
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

echo "== React Native TurboModule: syntax and type check (${cxx}) =="
if "${cxx}" "${flags[@]}" "${package}/cpp/PrinterDriverModule.cpp"; then
  echo "  ok    cpp/PrinterDriverModule.cpp"
else
  echo "  FAIL  cpp/PrinterDriverModule.cpp"
  failures=$((failures + 1))
fi

echo
echo "== Negative controls: each MUST fail, and fail for the stated reason =="

# Each control asserts on the compiler's diagnostic, not merely on a non-zero exit. "It
# failed" is not evidence; "it failed because the handle types do not match" is.
expect_rejection() {
  local label="$1" expected="$2" source="$3"
  shift 3
  local output
  if output="$("${cxx}" "$@" "${source}" 2>&1)"; then
    echo "  FAIL  ${label}: compiled -- this class of mistake is NOT being caught"
    failures=$((failures + 1))
    return
  fi
  if printf '%s' "${output}" | grep -qF "${expected}"; then
    echo "  ok    ${label}"
  else
    echo "  FAIL  ${label}: rejected, but not for the expected reason"
    echo "        expected to see: ${expected}"
    printf '%s\n' "${output}" | grep -m3 'error:' | sed 's/^/        got: /'
    failures=$((failures + 1))
  fi
}

# --- A. a pd_driver* handed to a function that takes a pd_printer* ----------------------
control_a="${temp_dir}/negative_handle_types.cpp"
{
  echo '#include "printerdriver/pd.h"'
  echo 'namespace pd_negative_control {'
  echo 'inline const char* idOfTheWrongHandle(pd_driver* driver) {'
  echo '  return pd_printer_id(driver);  // pd_driver* is not a pd_printer*'
  echo '}'
  echo '}'
} > "${control_a}"
expect_rejection "control A (pd_driver* passed to pd_printer_id)" \
  "'pd_driver *' to 'pd_printer *'" \
  "${control_a}" "${flags[@]}"

# --- B. a jsi::Value copied ---------------------------------------------------------------
control_b="${temp_dir}/negative_value_copy.cpp"
{
  echo '#include <utility>'
  echo 'namespace pd_negative_control {'
  echo 'inline facebook::jsi::Value duplicate(const facebook::jsi::Value& value) {'
  echo '  facebook::jsi::Value copy{value};  // jsi::Value is move-only'
  echo '  return copy;'
  echo '}'
  echo '}'
} > "${control_b}"
expect_rejection "control B (jsi::Value copied)" \
  "call to deleted constructor of 'facebook::jsi::Value'" \
  "${control_b}" "${flags[@]}"

# --- C. the module without the stub, i.e. the RN headers really are required --------------
expect_rejection "control C (no stub: the React Native headers are genuinely required)" \
  "'ReactCommon/CallInvoker.h' file not found" \
  "${package}/cpp/PrinterDriverModule.cpp" \
  -std=c++17 -fsyntax-only -I "${package}/cpp" -I "${repo}/capi/include"

echo
if [ "${failures}" -eq 0 ]; then
  echo "React Native TurboModule: syntax-checked against the real pd.h."
  echo "NOT built, NOT linked, NOT run, NOT loaded by any React Native app."
  exit 0
fi
echo "React Native TurboModule: ${failures} check(s) failed."
exit 1
