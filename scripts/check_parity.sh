#!/usr/bin/env sh
#
# M17 -- the wrapper parity contract (docs/api.md §17), enforced instead of trusted.
#
# THE RULE
#   Every capability of the C++ core is available in every wrapper, through an idiomatic
#   interface. No wrapper is a subset. The chain is C++ core -> C ABI (pd.h) -> Swift,
#   Dart, .NET, Kotlin, React Native, and a `pd_` function added to the ABI without a
#   binding in all five wrappers is an incomplete change, not a follow-up.
#
# WHAT THIS PROVES
#   1. Every public function declared in capi/include/printerdriver/pd.h is referenced
#      from every wrapper's SOURCE tree -- not its tests, which can bind the ABI directly
#      and would hide a missing public surface. A wrapper whose source tree is split
#      across languages names every part of it: the React Native package binds the ABI
#      from a C++ TurboModule (cpp/) and presents it from TypeScript (src/), so both
#      directories are its source tree and neither alone is.
#   2. Every exemption is written down. A wrapper may satisfy a C function through a
#      property or a higher-level member (a C getter that becomes a Swift property, an
#      index reader a callback-capable runtime does not need); those cases live in
#      scripts/parity_allowlist.txt with the member that covers them, so the mapping is
#      visible rather than silently absent.
#   3. The extraction itself is not lying. When a built C ABI library is present, every
#      pd_ symbol it exports must appear in the list parsed out of the header, which is
#      the check on the check: a declaration this script fails to see would otherwise
#      quietly excuse itself from parity.
#
# WHAT THIS DOES NOT PROVE
#   That a binding is correct, idiomatic, or reachable from application code. A reference
#   is a reference. The wrapper test suites (swift test, dart test, dotnet test) and
#   scripts/check_kotlin_syntax.sh are what prove behaviour; this proves coverage, which
#   is the thing that rots silently between milestones.
#
# NO FRAGILE MATCHING
#   Names are compared as whole tokens against a sorted set, never with substring greps:
#   `pd_print` is not satisfied by the text `pd_printer_id`. The header is parsed with
#   comments stripped and function-pointer typedefs excluded, so a name that only appears
#   in prose does not count as a declaration.
#
# THE NEGATIVE CONTROL
#   A green check nobody has seen go red is worth nothing. `--self-test` runs four
#   controls and requires the expected colour from each:
#     A. the plain run is green;
#     B. a function nobody has ever bound is injected into the required list, and the
#        check must go red naming all five wrappers;
#     C. one real binding is hidden from one wrapper, and the check must go red naming
#        that wrapper and only that one;
#     D. the same, aimed at the multi-directory wrapper. C alone would still pass if the
#        React Native lane silently matched everything or nothing, because it never
#        touches it; D is what proves that lane extracts a real per-wrapper set.
#
# Usage:
#   scripts/check_parity.sh              the check; non-zero when a wrapper is short
#   scripts/check_parity.sh --list       print the parsed pd_ function list and stop
#   scripts/check_parity.sh --self-test  run the negative controls above
#
# Test hooks (used only by --self-test; unset in a normal run):
#   PD_PARITY_INJECT_MISSING  a name appended to the required list
#   PD_PARITY_DROP            "wrapper:pd_function" pairs hidden from a wrapper's set

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

header="capi/include/printerdriver/pd.h"
allowlist="scripts/parity_allowlist.txt"

# name:source-directory[,more-directories]. The source tree only: a wrapper's tests bind
# the ABI directly and would satisfy every line of this check while the public surface
# stayed empty.
#
# reactnative carries two directories because its source tree really is two: cpp/ is the
# TurboModule that calls pd.h, src/ is the TypeScript API an app imports. Listing only cpp/
# would grade a wrapper by its glue; listing only src/ would look for C function names in a
# language that never spells them. Neither directory is a test directory -- the package's
# own tests live in wrappers/react-native/test/, which is not listed here.
wrappers="swift:wrappers/swift/Sources
dart:wrappers/dart/lib
dotnet:wrappers/dotnet/PrinterDriver
kotlin:wrappers/android/src/main
reactnative:wrappers/react-native/cpp,wrappers/react-native/src"

# --- 0. The negative control ------------------------------------------------------------
#
# Run before anything else and entirely through this same script, so what is proven red is
# the check itself and not a copy of it.

if [ "${1:-}" = "--self-test" ]; then
  self="$root/scripts/check_parity.sh"
  fake="pd_negative_control_never_bound"
  status=0

  echo "== control A: the plain run must be green"
  if "$self" > /dev/null 2>&1; then
    echo "   ok"
  else
    echo "   FAILED: the check is red before any control was applied. Run it directly."
    status=1
  fi

  echo "== control B: an unbound function must fail every wrapper"
  outb=$(PD_PARITY_INJECT_MISSING="$fake" "$self" 2>&1)
  if [ $? -eq 0 ]; then
    echo "   FAILED: injecting '$fake' did not turn the check red"
    status=1
  else
    named=$(echo "$outb" | grep -c "unbound: $fake")
    if [ "$named" -eq 5 ]; then
      echo "   ok: red, and all 5 wrappers named it"
    else
      echo "   FAILED: red, but $named of 5 wrappers reported it unbound"
      echo "$outb" | sed 's/^/      /'
      status=1
    fi
  fi

  echo "== control C: hiding one real binding must fail that wrapper alone"
  outc=$(PD_PARITY_DROP="swift:pd_print" "$self" 2>&1)
  if [ $? -eq 0 ]; then
    echo "   FAILED: hiding pd_print from the Swift sources did not turn the check red"
    status=1
  else
    hits=$(echo "$outc" | grep -c "unbound: pd_print$")
    if [ "$hits" -eq 1 ]; then
      echo "   ok: red for swift, green for the other four"
    else
      echo "   FAILED: expected exactly one wrapper to report it, got $hits"
      echo "$outc" | sed 's/^/      /'
      status=1
    fi
  fi

  # Control C never touches the one wrapper whose source tree is more than one directory,
  # so on its own it would stay green if that lane matched everything or nothing. This is
  # the same control aimed at it.
  echo "== control D: the same, for the wrapper assembled from two directories"
  outd=$(PD_PARITY_DROP="reactnative:pd_render_document" "$self" 2>&1)
  if [ $? -eq 0 ]; then
    echo "   FAILED: hiding pd_render_document from the React Native sources stayed green"
    status=1
  else
    hits=$(echo "$outd" | grep -c "unbound: pd_render_document$")
    if [ "$hits" -eq 1 ]; then
      echo "   ok: red for reactnative, green for the other four"
    else
      echo "   FAILED: expected exactly one wrapper to report it, got $hits"
      echo "$outd" | sed 's/^/      /'
      status=1
    fi
  fi

  echo
  if [ "$status" -eq 0 ]; then
    echo "== self-test: the parity check goes red when it should and green when it should"
  else
    echo "== self-test FAILED"
  fi
  exit "$status"
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

# --- 1. The required list ---------------------------------------------------------------
#
# Comments are stripped first (a name in prose is not a declaration), then the header is
# read as `;`-terminated statements. A statement that begins with `typedef` is skipped,
# which removes every function-pointer typedef -- pd_job_event_cb and friends are callback
# types a wrapper implements, not functions it calls -- and every struct and enum body.
# What is left declares a function when it contains `pd_name(` whose name is not preceded
# by `*` (the `(*pd_x_fn)(...)` shape) or by another identifier character.

strip_comments() {
  awk '
  {
    line = $0; out = ""
    while (length(line) > 0) {
      if (incomment) {
        p = index(line, "*/")
        if (p == 0) { line = ""; break }
        incomment = 0; line = substr(line, p + 2); continue
      }
      p = index(line, "/*"); q = index(line, "//")
      if (q > 0 && (p == 0 || q < p)) { out = out substr(line, 1, q - 1); line = ""; break }
      if (p == 0) { out = out line; line = ""; break }
      out = out substr(line, 1, p - 1)
      line = substr(line, p + 2)
      incomment = 1
    }
    print out
  }' "$1"
}

strip_comments "$header" | grep -v '^[[:space:]]*#' | awk '
  BEGIN { RS = ";" }
  {
    s = $0
    gsub(/[\n\t]+/, " ", s)
    sub(/^ +/, "", s); sub(/ +$/, "", s)
    if (s ~ /^typedef/) next
    rest = s
    while (match(rest, /pd_[A-Za-z0-9_]*[ \t]*\(/)) {
      name = substr(rest, RSTART, RLENGTH)
      sub(/[ \t]*\($/, "", name)
      pre = (RSTART > 1) ? substr(rest, RSTART - 1, 1) : " "
      if (pre != "*" && pre !~ /[A-Za-z0-9_]/) { print name; break }
      rest = substr(rest, RSTART + RLENGTH)
    }
  }' | sort -u > "$work/required"

if [ -n "${PD_PARITY_INJECT_MISSING:-}" ]; then
  echo "$PD_PARITY_INJECT_MISSING" >> "$work/required"
  sort -u "$work/required" > "$work/required.tmp" && mv "$work/required.tmp" "$work/required"
fi

required_count=$(wc -l < "$work/required" | tr -d ' ')

if [ "${1:-}" = "--list" ]; then
  cat "$work/required"
  exit 0
fi

if [ "$required_count" -lt 50 ]; then
  echo "check_parity: only $required_count functions parsed out of $header -- the parser"
  echo "              is broken, not the header. Refusing to report parity from it."
  exit 1
fi

# --- 2. The check on the parser ---------------------------------------------------------
#
# Every pd_ symbol a built C ABI library exports must be in the list above. A declaration
# the parser misses would otherwise excuse itself from parity for good.

symbol_check=""
if command -v nm >/dev/null 2>&1; then
  for lib in build/libprinterdriver_capi.a build-*/libprinterdriver_capi.a \
             build/libprinterdriver_capi.* build-*/libprinterdriver_capi.*; do
    [ -f "$lib" ] || continue
    nm -g "$lib" 2>/dev/null | awk '$2 == "T" { print $3 }' \
      | sed 's/^_//' | grep -E '^pd_[a-z0-9_]+$' | sort -u > "$work/symbols"
    [ -s "$work/symbols" ] || continue
    unparsed=$(comm -23 "$work/symbols" "$work/required")
    if [ -n "$unparsed" ]; then
      echo "check_parity: $lib exports pd_ symbols the header parser did not find:"
      echo "$unparsed" | sed 's/^/   /'
      exit 1
    fi
    symbol_check="$lib"
    break
  done
fi

# --- 3. The allowlist -------------------------------------------------------------------
#
# Format, one exemption per line:  <wrapper|*>  <pd_function>  <covering member -- why>
# Blank lines and lines beginning with # are ignored.

: > "$work/allow"
if [ -f "$allowlist" ]; then
  awk 'NF && $1 !~ /^#/ { print }' "$allowlist" > "$work/allow"
fi

stale=0
while IFS= read -r line; do
  [ -n "$line" ] || continue
  aw=$(echo "$line" | awk '{ print $1 }')
  af=$(echo "$line" | awk '{ print $2 }')
  anote=$(echo "$line" | awk '{ $1 = ""; $2 = ""; sub(/^ +/, ""); print }')
  if ! grep -qxF "$af" "$work/required"; then
    echo "check_parity: $allowlist exempts '$af', which $header does not declare."
    stale=1
  fi
  if [ "$aw" != "*" ] && ! echo "$wrappers" | grep -q "^$aw:"; then
    echo "check_parity: $allowlist names unknown wrapper '$aw'."
    stale=1
  fi
  if [ -z "$anote" ]; then
    echo "check_parity: $allowlist exempts $aw/$af with no covering member named."
    stale=1
  fi
done < "$work/allow"
[ "$stale" -eq 0 ] || exit 1

# --- 4. Per-wrapper coverage -------------------------------------------------------------

echo "== wrapper parity (docs/api.md §17)"
echo "   $header declares $required_count public pd_ functions"
if [ -n "$symbol_check" ]; then
  echo "   cross-checked against the symbols exported by $symbol_check"
else
  echo "   no built C ABI library found: the symbol cross-check was skipped"
fi
echo

failed=0
for entry in $wrappers; do
  name=${entry%%:*}
  dirs=$(echo "${entry#*:}" | tr ',' ' ')

  missing_dir=0
  for dir in $dirs; do
    [ -d "$dir" ] && continue
    echo "   $name: $dir does not exist"
    missing_dir=1
  done
  if [ "$missing_dir" -ne 0 ]; then
    failed=1
    continue
  fi

  # shellcheck disable=SC2086
  grep -rhoE 'pd_[A-Za-z0-9_]+' $dirs 2>/dev/null | sort -u > "$work/found.$name"

  # Test hook: hide a real binding from one wrapper, so the check can be watched going red.
  for drop in ${PD_PARITY_DROP:-}; do
    case "$drop" in
      "$name":*)
        token=${drop#*:}
        grep -vxF "$token" "$work/found.$name" > "$work/found.tmp" 2>/dev/null
        mv "$work/found.tmp" "$work/found.$name"
        ;;
    esac
  done

  awk -v w="$name" 'NF && ($1 == w || $1 == "*") { print $2 }' "$work/allow" \
    | sort -u > "$work/allow.$name"

  comm -23 "$work/required" "$work/found.$name" > "$work/absent.$name"
  comm -12 "$work/absent.$name" "$work/allow.$name" > "$work/exempt.$name"
  comm -23 "$work/absent.$name" "$work/allow.$name" > "$work/missing.$name"

  bound=$(comm -12 "$work/required" "$work/found.$name" | wc -l | tr -d ' ')
  exempt=$(wc -l < "$work/exempt.$name" | tr -d ' ')
  missing=$(wc -l < "$work/missing.$name" | tr -d ' ')

  if [ "$missing" -eq 0 ]; then
    echo "   $name ($(echo "$dirs" | sed 's/ /, /g'))"
    echo "      bound $bound/$required_count, allowlisted $exempt, missing 0  -- ok"
  else
    echo "   $name ($(echo "$dirs" | sed 's/ /, /g'))"
    echo "      bound $bound/$required_count, allowlisted $exempt, MISSING $missing"
    sed 's/^/         unbound: /' "$work/missing.$name"
    failed=1
  fi

  if [ "$exempt" -gt 0 ]; then
    while IFS= read -r fn; do
      note=$(awk -v w="$name" -v f="$fn" \
        'NF && ($1 == w || $1 == "*") && $2 == f { $1 = ""; $2 = ""; sub(/^ +/, ""); print; exit }' \
        "$work/allow")
      echo "         allowlisted: $fn -> $note"
    done < "$work/exempt.$name"
  fi

  # An exemption for something the wrapper does reference is not an error -- the wrapper
  # got better -- but it is stale documentation, so it is said out loud.
  comm -12 "$work/allow.$name" "$work/found.$name" | while IFS= read -r fn; do
    [ -n "$fn" ] || continue
    echo "         note: $fn is allowlisted but referenced anyway; the entry can go"
  done
done

echo
if [ "$failed" -eq 0 ]; then
  echo "== parity: every wrapper binds every pd_ function, or names what covers it"
  exit 0
fi
echo "== parity FAILED: see the unbound functions above."
echo "   Bind them in the wrapper, or add an allowlist line to $allowlist naming the"
echo "   member that already covers the capability idiomatically."
exit 1
