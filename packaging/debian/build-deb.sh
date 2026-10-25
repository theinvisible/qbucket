#!/usr/bin/env bash
#
# Builds the QBucket Debian package. Runs as root in a clean Ubuntu container
# (packaging/debian/build-in-docker.sh); works on any Debian-based system that
# has the build dependencies.
#
#   packaging/debian/build-deb.sh <repository> <output-dir>
#
# QBUCKET_JOBS: parallel compilers and tests (default: every core).
#
# The tree is copied first: dpkg-buildpackage writes next to the sources and
# drops the .deb in the parent directory, and the repository is a read-only
# mount or the developer's working copy. packaging/debian/ becomes debian/ in
# the copy. Build dependencies come from debian/control only.
set -euo pipefail

SRC="${1:?repository}"
OUT="${2:?output directory}"
log() { printf '==> %s\n' "$*"; }
die() { printf 'build-deb: %s\n' "$*" >&2; exit 1; }

SRC="$(cd "$SRC" && pwd)"
[[ -f "$SRC/packaging/debian/control" ]] || die "$SRC/packaging/debian/control not found"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
TREE="$WORK/qbucket"
mkdir -p "$TREE"

log "copying the sources"
# Build trees, IDE folders and packages stay behind; a stale CMakeCache.txt
# would make CMake refuse the out-of-source configure.
tar -C "$SRC" \
    --exclude='./.git' --exclude='./build' --exclude='./build-*' --exclude='./cmake-build-*' \
    --exclude='./dist' --exclude='./.idea' --exclude='./.flatpak-builder' --exclude='./out' \
    --exclude='CMakeCache.txt' --exclude='CMakeFiles' \
    -cf - . | tar -C "$TREE" -xf -
cp -r "$TREE/packaging/debian" "$TREE/debian"
rm -f "$TREE/debian/build-deb.sh" "$TREE/debian/build-in-docker.sh"

# One version everywhere (also checked by ctest version_consistency).
( cd "$TREE" && sh tools/check-version.sh ) || die "version mismatch"

if ! dpkg-checkbuilddeps "$TREE/debian/control" 2>/dev/null; then
    [[ "$(id -u)" -eq 0 ]] || die "build dependencies missing: $(dpkg-checkbuilddeps "$TREE/debian/control" 2>&1 || true)"
    log "installing build dependencies"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get build-dep -y -qq "$TREE"
fi

log "dpkg-buildpackage"
( cd "$TREE" && dpkg-buildpackage -b -uc -us "-j${QBUCKET_JOBS:-$(nproc)}" )

shopt -s nullglob
debs=()
for deb in "$WORK"/*.deb; do
    [[ "$deb" == *-dbgsym_* ]] && continue
    debs+=("$deb")
done
(( ${#debs[@]} == 1 )) || die "expected one .deb, got ${#debs[@]}"

if command -v lintian >/dev/null 2>&1; then
    log "lintian"
    # Errors fail the build; warnings are printed for a look.
    lintian --fail-on error --display-info "${debs[0]}"
fi

cp -f "${debs[0]}" "$OUT/"
log "$OUT/$(basename "${debs[0]}")"
