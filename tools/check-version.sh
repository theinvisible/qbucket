#!/bin/sh
# One version everywhere. The source is project(qbucket VERSION …) in
# CMakeLists.txt; the newest entry of CHANGELOG.md, the newest <release> of the
# AppStream metainfo, the Debian changelog (upstream part) and the man page
# must name it — and the same release date as CHANGELOG.md. The Windows
# installer and the rc file take the version from CMake. Run from the
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
date=$(sed -n 's/^## \[[0-9.]*\] — \([0-9-]*\).*/\1/p' CHANGELOG.md | head -n 1)
checkDate() {   # what, found
    if [ "$2" != "$date" ]; then
        echo "$1 is dated '$2', CHANGELOG.md '$date'"
        fail=1
    fi
}
metainfo=packaging/linux/io.github.theinvisible.QBucket.metainfo.xml
check CHANGELOG.md "$(sed -n 's/^## \[\([0-9.]*\)\].*/\1/p' CHANGELOG.md | head -n 1)"
check "the metainfo" "$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' "$metainfo" | head -n 1)"
checkDate "the metainfo" "$(sed -n 's/.*<release version="[^"]*" date="\([^"]*\)".*/\1/p' "$metainfo" | head -n 1)"
if [ -f packaging/debian/changelog ]; then
    check packaging/debian/changelog "$(sed -n '1s/^qbucket (\([^-)]*\).*/\1/p' packaging/debian/changelog)"
    # " -- Name <mail>  Fri, 30 Oct 2026 12:00:00 +0100" of the first entry
    checkDate packaging/debian/changelog "$(sed -n 's/^ -- .*>  [A-Za-z]*, \([0-9]*\) \([A-Za-z]*\) \([0-9]*\) .*/\3 \2 \1/p' \
        packaging/debian/changelog | head -n 1 | awk '{
            m = index("JanFebMarAprMayJunJulAugSepOctNovDec", $2)
            if (m) printf "%s-%02d-%02d", $1, (m + 2) / 3, $3 }')"
    check "the man page" "$(sed -n 's/^\.TH QBUCKET 1 "[^"]*" "qbucket \([^"]*\)".*/\1/p' packaging/debian/qbucket.1)"
    checkDate "the man page" "$(sed -n 's/^\.TH QBUCKET 1 "\([^"]*\)".*/\1/p' packaging/debian/qbucket.1)"
fi
[ "$fail" -eq 0 ] || exit 1
echo "version: $cmake of $date everywhere"
