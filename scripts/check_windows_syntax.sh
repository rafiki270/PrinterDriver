#!/usr/bin/env bash
#
# Syntax- and type-check the SDK's Windows edge on a host that is not Windows.
#
# WHAT THIS PROVES
#   Every Winsock2/Win32 call in the Windows-only sources has the right name, arity and
#   argument types against core/tests/win32_stub.h, a hand-written stand-in for
#   <winsock2.h>/<ws2tcpip.h>/<windows.h> whose signatures mirror the real ones in the
#   ways that matter (SOCKET is UINT_PTR, HANDLE is void*, setsockopt takes char*,
#   send/recv take and return int, addrlen is int and not socklen_t). It also proves the
#   Windows branches inside the shared headers and the test harness parse and type-check.
#
# WHAT THIS DOES NOT PROVE
#   Nothing about behaviour. Nothing about MSVC or clang-cl. Nothing about linking
#   against ws2_32. Nothing about the real Windows SDK headers, which are
#   ABI-compatible with this stub in the parts used here but were not compared to it
#   byte for byte. The first behavioural evidence will come from
#   .github/workflows/windows.yml, which is manual-dispatch only and has not been run.
#
# THE NEGATIVE CONTROLS
#   A green check is worthless if the check cannot go red, so this script also runs
#   three deliberate failures and requires each one to fail:
#     A. a Winsock SOCKET narrowed into an int -- the canonical Windows port bug, and
#        the exact thing win32_stub.h's UINT_PTR typedef exists to catch;
#     B. FlushFileBuffers called on a std::string instead of a file handle;
#     C. pd::net::Socket initialised from a 64-bit handle WITHOUT
#        -DPD_FORCE_WINDOWS_PLATFORM, which must fail because the POSIX branch of
#        net_platform.hpp makes Socket a plain `int` -- the mirror image of control A,
#        so the two bracket the selector from both sides and prove it really selects.
#
# Usage: scripts/check_windows_syntax.sh   (from anywhere; paths are resolved from the
# repository root, which is this script's parent directory.)

set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-clang++}"

flags=(
  -std=c++17
  -fsyntax-only
  -Wall -Wextra -Wpedantic
  -DPD_WINDOWS_SYNTAX_CHECK
  -include "${root}/core/tests/win32_stub.h"
  -I "${root}/core/include"
  -I "${root}/core/src"
  -I "${root}/core/tests"
  -I "${root}/capi/include"
  -I "${root}/queue/include"
  -I "${root}/dsl/include"
  -I "${root}/agent/include"
)

# Every translation unit that either is Windows-only or contains a Windows branch.
# discovery.cpp and the agent's HTTP server are here because both open sockets directly
# through printerdriver/net_platform.hpp rather than through TcpTransport.
sources=(
  core/src/transport_win.cpp
  core/src/platform_file_win.cpp
  core/src/job_store.cpp
  core/src/capability_probe.cpp
  core/src/discovery.cpp
  agent/src/http.cpp
  agent/src/agent.cpp
  core/tests/test_engine.cpp
  core/tests/test_store.cpp
)

failures=0
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

echo "== Windows edge: syntax and type check (${cxx}) =="
for source in "${sources[@]}"; do
  if "${cxx}" "${flags[@]}" -DPD_FORCE_WINDOWS_PLATFORM "${root}/${source}"; then
    echo "  ok    ${source}"
  else
    echo "  FAIL  ${source}"
    failures=$((failures + 1))
  fi
done

echo
echo "== Negative controls: each MUST fail, and fail for the stated reason =="

# Each control asserts on the compiler's diagnostic, not merely on a non-zero exit. "It
# failed" is not evidence; "it failed because the SOCKET could not be narrowed" is.
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

# --- A. a SOCKET narrowed into an int ------------------------------------------------
control_a="${temp_dir}/negative_socket_to_int.cpp"
{
  echo '#include "printerdriver/net_platform.hpp"'
  echo 'namespace pd_negative_control {'
  echo 'inline int truncateSocket(pd::net::Socket socket) {'
  echo '  int fd{socket};  // narrowing UINT_PTR -> int: ill-formed, and the real bug'
  echo '  return fd;'
  echo '}'
  echo '}'
  cat "${root}/core/src/transport_win.cpp"
} > "${control_a}"
expect_rejection "control A (SOCKET narrowed to int)" \
  "cannot be narrowed from type 'pd::net::Socket'" \
  "${control_a}" "${flags[@]}" -DPD_FORCE_WINDOWS_PLATFORM

# --- B. a Win32 call given the wrong argument type ------------------------------------
control_b="${temp_dir}/negative_wrong_handle.cpp"
{
  echo '#include <string>'
  echo '#include "platform_file.hpp"'
  echo 'namespace pd_negative_control {'
  echo 'inline bool flushAString(const std::string& not_a_handle) {'
  echo '  return ::FlushFileBuffers(not_a_handle) != 0;  // wrong argument type'
  echo '}'
  echo '}'
  cat "${root}/core/src/platform_file_win.cpp"
} > "${control_b}"
expect_rejection "control B (FlushFileBuffers on a std::string)" \
  "to 'HANDLE'" \
  "${control_b}" "${flags[@]}" -DPD_FORCE_WINDOWS_PLATFORM

# --- C. the selector macro actually selects -------------------------------------------
# The original control here compiled core/src/transport_win.cpp as an ordinary POSIX
# translation unit and expected an undeclared `socket_`. But that whole file is wrapped in
# a platform guard, so as POSIX it preprocesses to an EMPTY translation unit that compiles
# cleanly -- the control could never go red, and "compiled" was scored as a failure that
# proved nothing. A single platform macro cannot make the Windows *body* visible while
# leaving the *members* POSIX, because both derive from that one macro, so the old premise
# was unreachable by construction.
#
# Assert the selector directly instead, and symmetrically with control A. Compiled as
# POSIX (no stub, no PD_FORCE_WINDOWS_PLATFORM), pd::net::Socket is a plain `int`, so
# initialising one from a 64-bit handle is a narrowing conversion and ill-formed. Under
# the Windows selector Socket is the pointer-width Winsock handle and the same line
# compiles -- exactly the inverse of control A's SOCKET-narrowed-to-int, so the pair
# brackets the selector from both directions and each half fails only in its own config.
control_c="${temp_dir}/negative_selector.cpp"
{
  echo '#include "printerdriver/net_platform.hpp"'
  echo 'namespace pd_negative_control {'
  echo 'inline pd::net::Socket fromWideHandle(unsigned long long handle) {'
  echo '  pd::net::Socket socket{handle};  // 64-bit -> int narrowing: ill-formed under POSIX'
  echo '  return socket;'
  echo '}'
  echo '}'
} > "${control_c}"
expect_rejection "control C (no PD_FORCE_WINDOWS_PLATFORM: POSIX Socket is int, not the handle)" \
  "cannot be narrowed from type 'unsigned long long' to 'pd::net::Socket'" \
  "${control_c}" \
  -std=c++17 -fsyntax-only -I "${root}/core/include" -I "${root}/core/src"

echo
if [ "${failures}" -eq 0 ]; then
  echo "Windows edge: syntax-checked. NOT built, NOT linked, NOT run."
  exit 0
fi
echo "Windows edge: ${failures} check(s) failed."
exit 1
