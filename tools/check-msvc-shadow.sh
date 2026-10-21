#!/bin/sh
# MSVC's C4458 ("declaration hides class member") breaks the Windows build
# under /WX, and GCC says nothing. clang's -Wshadow-all finds the same cases
# as "shadows a field"; its "uncaptured local" hits (init-captures such as
# `alive = alive`, lambda parameters named like a member) are fine on MSVC
# and ignored here.
#
#   tools/check-msvc-shadow.sh BUILD_DIR FILE...
#
# CLANG_TIDY picks the binary (default: clang-tidy). Exits non-zero on a
# hidden member, and when clang-tidy did not check a file at all.
set -eu
build=$1
shift
tidy=${CLANG_TIDY:-clang-tidy}
status=0
for f in "$@"; do
    # cert-dcl58-cpp only keeps clang-tidy from refusing "no checks enabled".
    rc=0
    out=$("$tidy" -p "$build" -checks='-*,cert-dcl58-cpp,clang-diagnostic-*' \
          --extra-arg=-Wshadow-all --extra-arg=-Wno-error "$f" 2>&1) || rc=$?
    [ $rc -eq 0 ] && continue
    # A failure without a single diagnostic: clang-tidy itself failed.
    if ! printf '%s\n' "$out" | grep -q -e ': error:' -e ': warning:'; then
        echo "$f: clang-tidy did not run (exit $rc):"
        printf '%s\n' "$out" | head -3
        status=1
        continue
    fi
    # Lambda parameters and init-captures land in -Wshadow-uncaptured-local,
    # also when they share a member's name: MSVC accepts those.
    hits=$(printf '%s\n' "$out" | grep 'shadows a field' | grep -v 'shadow-uncaptured-local' || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits"
        status=1
    fi
done
[ $status -eq 0 ] && echo "msvc shadow: ok ($# files)"
exit $status
