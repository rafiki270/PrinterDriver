#!/usr/bin/env bash
#
# Syntax- and type-check the SDK's Linux Bluetooth edge on a host without BlueZ.
#
# WHAT THIS PROVES
#   Every BlueZ call in core/src/transport_bluez.cpp has the right name, arity and
#   argument types against core/tests/linux_bluetooth_stub/bluetooth/*.h, hand-written
#   stand-ins for <bluetooth/bluetooth.h> and <bluetooth/rfcomm.h> whose declarations
#   mirror the real ones in the ways that matter (bdaddr_t is six packed bytes rather
#   than an integer, sockaddr_rc carries rc_family/rc_bdaddr/rc_channel in that order,
#   str2ba/ba2str take the pointer arguments BlueZ takes, BTPROTO_RFCOMM is the third
#   argument to socket()).
#
# WHAT THIS DOES NOT PROVE
#   Nothing about behaviour. Nothing about real BlueZ headers or libbluetooth. Nothing
#   about linking. Nothing about pairing, channel discovery or an actual printer. A
#   paired RFCOMM device on a real Linux box is the only thing that can turn this into
#   evidence, and it has not been run against one.
#
# THE NEGATIVE CONTROLS
#   A green check is worthless if the check cannot go red, so this script also runs
#   three deliberate failures and requires each one to fail for the stated reason:
#     A. a bdaddr_t passed by value where BlueZ wants a pointer -- the commonest BlueZ
#        mistake, and the exact thing the packed-struct typedef exists to catch;
#     B. rc_channel assigned a bdaddr_t -- the address/channel transposition that a
#        pair of integers would have accepted silently;
#     C. the whole check run WITHOUT -DPD_FORCE_LINUX_BLUETOOTH, which must produce an
#        empty translation unit -- i.e. proof that the guard really guards, which is
#        what keeps SwiftPM (it compiles every file in core/src) green on iOS.
#
# Usage: scripts/check_linux_bluetooth_syntax.sh

set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-clang++}"

flags=(
  -std=c++17
  -fsyntax-only
  -Wall -Wextra -Wpedantic
  -I "${root}/core/tests/linux_bluetooth_stub"
  -I "${root}/core/include"
  -I "${root}/core/src"
)

failures=0
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

echo "== Linux Bluetooth edge: syntax and type check (${cxx}) =="
if "${cxx}" "${flags[@]}" -DPD_FORCE_LINUX_BLUETOOTH "${root}/core/src/transport_bluez.cpp"; then
  echo "  ok    core/src/transport_bluez.cpp"
else
  echo "  FAIL  core/src/transport_bluez.cpp"
  failures=$((failures + 1))
fi

echo
echo "== Negative controls: each MUST fail, and fail for the stated reason =="

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

# --- A. bdaddr_t passed by value to str2ba -------------------------------------------
control_a="${temp_dir}/negative_bdaddr_by_value.cpp"
{
  echo '#include <bluetooth/bluetooth.h>'
  echo 'namespace pd_negative_control {'
  echo 'inline int parseByValue(const char* text) {'
  echo '  bdaddr_t address;'
  echo '  return str2ba(text, address);  // BlueZ wants bdaddr_t*, not bdaddr_t'
  echo '}'
  echo '}'
  cat "${root}/core/src/transport_bluez.cpp"
} > "${control_a}"
expect_rejection "control A (bdaddr_t passed by value to str2ba)" \
  "to 'bdaddr_t *'" \
  "${control_a}" "${flags[@]}" -DPD_FORCE_LINUX_BLUETOOTH

# --- B. the address/channel transposition ---------------------------------------------
control_b="${temp_dir}/negative_channel_transposed.cpp"
{
  echo '#include <bluetooth/rfcomm.h>'
  echo 'namespace pd_negative_control {'
  echo 'inline void transpose(const bdaddr_t& address) {'
  echo '  struct sockaddr_rc target;'
  echo '  target.rc_channel = address;  // channel is a uint8_t, address is six bytes'
  echo '}'
  echo '}'
  cat "${root}/core/src/transport_bluez.cpp"
} > "${control_b}"
expect_rejection "control B (bdaddr_t assigned to rc_channel)" \
  "from incompatible type 'const bdaddr_t'" \
  "${control_b}" "${flags[@]}" -DPD_FORCE_LINUX_BLUETOOTH

# --- C. the guard actually guards ------------------------------------------------------
# Without the selector this file must contribute nothing at all. Proved by compiling it
# with the stub include path REMOVED: if a single BlueZ declaration escaped the guard,
# the missing <bluetooth/bluetooth.h> would fail the compile. It succeeding is the proof
# that SwiftPM's unconditional compile of core/src stays green on iOS and macOS.
echo
echo "== Guard control: the file must be empty without the selector =="
if "${cxx}" -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
    -I "${root}/core/include" -I "${root}/core/src" \
    "${root}/core/src/transport_bluez.cpp"; then
  echo "  ok    control C (no selector: empty translation unit, no BlueZ headers needed)"
else
  echo "  FAIL  control C: the file is not self-guarding -- this breaks the iOS build"
  failures=$((failures + 1))
fi

echo
if [ "${failures}" -eq 0 ]; then
  echo "all checks passed -- syntax and types only, never run against real BlueZ"
  exit 0
fi
echo "${failures} check(s) failed"
exit 1
