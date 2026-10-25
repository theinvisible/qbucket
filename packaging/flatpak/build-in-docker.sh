#!/usr/bin/env bash
#
# The Flatpak bundle, built, linted and smoke-tested in Docker — nothing is
# installed on the host.
#
#   packaging/flatpak/build-in-docker.sh [output-dir]     (default: dist/)
#
# 1. Build: Flathub's builder image for the manifest's KDE runtime builds the
#    manifest (the test suite runs in the build sandbox), exports a repo and
#    a single-file bundle, and runs flatpak-builder-lint on the manifest and
#    the repo (its errors fail the build, but for the three a private
#    repository can't avoid).
# 2. Smoke test (packaging/flatpak/smoke.Dockerfile, smoke.sh): a Fedora
#    container installs the KDE runtime from Flathub and the bundle, then
#    runs the app inside its sandbox: --version, the window (offscreen), and
#    --self-test against the Secret portal (xdg-desktop-portal + GNOME
#    Keyring) — whether the keychain works with the manifest's finish-args.
#
# Both need --privileged (bubblewrap). Caches: Docker volumes
# qbucket-flatpak-builder (sources, build, repo and flatpak-builder's state)
# and qbucket-flatpak-smoke
# (the runtime in the smoke container).
#
# The build runs QBUCKET_JOBS compilers at a time (default 12) in a container
# limited to QBUCKET_DOCKER_MEMORY (default 20g): with every core next to a
# desktop the machine can hang; at the limit the OOM killer ends the build.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$REPO/dist}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
APP_ID=io.github.theinvisible.QBucket
MANIFEST=packaging/flatpak/$APP_ID.yml
RUNTIME="$(sed -n "s/^runtime-version: '\(.*\)'/\1/p" "$REPO/$MANIFEST")"
VERSION="$(sed -n '/^project(qbucket/,/)/s/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")"
BUNDLE="qbucket-$VERSION-x86_64.flatpak"
BUILDER="ghcr.io/flathub-infra/flatpak-github-actions:kde-$RUNTIME"
JOBS="${QBUCKET_JOBS:-12}"
MEMORY="${QBUCKET_DOCKER_MEMORY:-20g}"
log() { printf '\n==> %s\n' "$*"; }

log "build ($BUILDER)"
docker run --rm --privileged --memory="$MEMORY" --memory-swap="$MEMORY" \
    -v "$REPO:/src:ro" -v "$OUT:/out" -v qbucket-flatpak-builder:/cache \
    -e JOBS="$JOBS" -e APP_ID="$APP_ID" -e MANIFEST="$MANIFEST" -e BUNDLE="$BUNDLE" -e OWNER="$(id -u):$(id -g)" \
    --entrypoint bash "$BUILDER" -euc '
    # The manifest takes the sources from ../.. — a copy without build trees.
    # Everything in the volume: flatpak-builder wants its state and build
    # directories on one file system (and the state cached between runs).
    rm -rf /cache/src /cache/build /cache/repo
    mkdir -p /cache/src
    tar -C /src --exclude=./.git --exclude="./build" --exclude="./build-*" \
        --exclude="./cmake-build-*" --exclude=./dist --exclude=./.flatpak-builder \
        -cf - . | tar -C /cache/src -xf -
    cd /cache/src
    flatpak-builder --disable-rofiles-fuse --force-clean --jobs="$JOBS" --state-dir=/cache/state \
        --repo=/cache/repo /cache/build "$MANIFEST"
    flatpak build-bundle /cache/repo "/out/$BUNDLE" "$APP_ID" \
        --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
    chown "$OWNER" "/out/$BUNDLE"
    # Errors fail the build, except those of a private repository: its URLs
    # (homepage, screenshots) are unreachable, and Flathub mirrors the
    # screenshots into the repo only on its own builders.
    known="appid-url-not-reachable appstream-missing-screenshots appstream-screenshots-not-mirrored-in-ostree"
    lint() {
        echo "--- flatpak-builder-lint $1"
        out=$(flatpak-builder-lint --exceptions "$@" || true)
        echo "$out" | jq -c "{errors, warnings}"
        bad=$(echo "$out" | jq -r ".errors // [] | .[]" | grep -vxF -e ${known// / -e } || true)
        if [ -n "$bad" ]; then echo "flatpak-builder-lint $1: $bad"; return 1; fi
    }
    lint manifest "$MANIFEST"
    lint repo /cache/repo'

log "smoke test ($BUNDLE)"
docker build -q -t qbucket-flatpak-smoke -f "$REPO/packaging/flatpak/smoke.Dockerfile" \
    "$REPO/packaging/flatpak" >/dev/null
docker run --rm --privileged \
    -v "$OUT:/out:ro" -v qbucket-flatpak-smoke:/var/lib/flatpak \
    -v "$REPO/packaging/flatpak/smoke.sh:/smoke.sh:ro" \
    qbucket-flatpak-smoke bash /smoke.sh "/out/$BUNDLE" "$APP_ID" "$(sed -n 's/^runtime: //p' "$REPO/$MANIFEST")//$RUNTIME"

log "ok: $OUT/$BUNDLE"
