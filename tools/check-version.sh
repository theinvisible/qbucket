#!/bin/sh
# One version everywhere. The source is project(qbucket VERSION …) in
# CMakeLists.txt; the newest entry of CHANGELOG.md, the newest <release> of the
# AppStream metainfo and the Debian changelog (upstream part) must name it.
# The Windows installer and the rc file take it from CMake. Run from the
# repository root.
set -eu
cmake=$(sed -n '/^project(qbucket/,/)/s/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$cmake" ] || { echo "no VERSION in CMakeLists.txt"; exit 1; }
fail=0
check() {   # what, found
    if [ "$2" != "$cmake" ]; then
        echo "$1 says '$2', CMakeLists.txt says '$cmake'"
        fail=1
    fi
}
check CHANGELOG.md "$(sed -n 's/^## \[\([0-9.]*\)\].*/\1/p' CHANGELOG.md | head -n 1)"
check "the metainfo" "$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' \
    packaging/linux/io.github.theinvisible.QBucket.metainfo.xml | head -n 1)"
if [ -f packaging/debian/changelog ]; then
    check packaging/debian/changelog "$(sed -n '1s/^qbucket (\([^-)]*\).*/\1/p' packaging/debian/changelog)"
    check "the man page" "$(sed -n 's/^\.TH QBUCKET 1 "[^"]*" "qbucket \([^"]*\)".*/\1/p' packaging/debian/qbucket.1)"
fi
[ "$fail" -eq 0 ] || exit 1
echo "version: $cmake everywhere"
