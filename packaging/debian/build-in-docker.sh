#!/usr/bin/env bash
#
# The .deb for Ubuntu 26.04, built and tested in Docker — nothing is
# installed on the host.
#
#   packaging/debian/build-in-docker.sh [output-dir]     (default: dist/)
#
# 1. Build: a clean ubuntu:26.04 installs the build dependencies, builds the
#    package with the whole test suite (headless) and runs lintian.
# 2. Install: another clean ubuntu:26.04 installs the .deb with apt (its
#    Depends must be enough), then checks
#      - `qbucket --version`,
#      - the window starts (offscreen, killed after 5 s),
#      - `qbucket --self-test` without a keychain — no session bus, and a
#        bus without a Secret Service (exit 1, "no backend": the app falls
#        back to session-only secrets) — and with an unlocked GNOME Keyring
#        on a D-Bus session (exit 0),
#      - the installed .desktop file and AppStream metainfo validate.
# apt's downloads are kept in the Docker volume qbucket-deb-apt.
#
# The build runs QBUCKET_JOBS compilers and tests at a time (default 12) in a
# container limited to QBUCKET_DOCKER_MEMORY (default 20g): with every core
# (-j20) next to a desktop this laptop hung; at the limit the OOM killer
# ends the build, not the machine.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$REPO/dist}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
IMAGE=ubuntu:26.04
JOBS="${QBUCKET_JOBS:-12}"
MEMORY="${QBUCKET_DOCKER_MEMORY:-20g}"
log() { printf '\n==> %s\n' "$*"; }

# Keep the downloaded packages between runs (the image deletes them).
APT=(-v qbucket-deb-apt:/var/cache/apt/archives
     -e DEBIAN_FRONTEND=noninteractive -e LANG=C.UTF-8)
NOCLEAN='rm -f /etc/apt/apt.conf.d/docker-clean'

log "build ($IMAGE)"
docker run --rm --memory="$MEMORY" --memory-swap="$MEMORY" "${APT[@]}" -e QBUCKET_JOBS="$JOBS" \
    -v "$REPO:/src:ro" -v "$OUT:/out" "$IMAGE" bash -euc "
    $NOCLEAN
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends dpkg-dev build-essential lintian >/dev/null
    /src/packaging/debian/build-deb.sh /src /out
    chown $(id -u):$(id -g) /out/qbucket_*.deb"

DEB="$(cd "$OUT" && ls -t qbucket_*_amd64.deb | head -n 1)"
[[ -n "$DEB" ]] || { echo "no .deb in $OUT" >&2; exit 1; }

log "install and smoke test ($DEB on a clean $IMAGE)"
docker run --rm "${APT[@]}" -v "$OUT:/out:ro" "$IMAGE" bash -euc "
    $NOCLEAN
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends /out/$DEB >/dev/null
    echo '--- qbucket --version'
    qbucket --version
    echo '--- the window starts (offscreen)'
    rc=0; QT_QPA_PLATFORM=offscreen timeout 5 qbucket || rc=\$?
    [ \$rc -eq 124 ] || { echo \"qbucket exited with \$rc before the timeout\"; exit 1; }
    nobackend() {   # what, command...
        what=\$1; shift
        echo \"--- self-test, \$what (must say: no backend)\"
        rc=0; out=\$(\"\$@\" 2>&1) || rc=\$?
        echo \"\$out\"
        [ \$rc -eq 1 ] && echo \"\$out\" | grep -q 'Keychain: *write: no backend' \
            || { echo \"expected exit 1 with no backend, got \$rc\"; exit 1; }
    }
    nobackend 'no session bus' qbucket --self-test
    apt-get install -y -qq --no-install-recommends dbus dbus-daemon gnome-keyring \
        desktop-file-utils appstream >/dev/null
    # gnome-keyring is activatable now: a bus without the system's service
    # files has no Secret Service at all.
    cat > /tmp/bare-bus.conf <<'CONF'
<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"
 \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context=\"default\">
    <allow send_destination=\"*\" eavesdrop=\"true\"/>
    <allow eavesdrop=\"true\"/>
    <allow own=\"*\"/>
  </policy>
</busconfig>
CONF
    nobackend 'a bus without a Secret Service' \
        dbus-run-session --config-file=/tmp/bare-bus.conf -- qbucket --self-test
    echo '--- self-test with an unlocked GNOME Keyring'
    dbus-run-session -- sh -ec '
        printf test | gnome-keyring-daemon --unlock --components=secrets >/dev/null
        qbucket --self-test'
    echo '--- desktop file and metainfo'
    desktop-file-validate /usr/share/applications/io.github.theinvisible.QBucket.desktop
    appstreamcli validate --no-net --pedantic \
        /usr/share/metainfo/io.github.theinvisible.QBucket.metainfo.xml"

log "ok: $OUT/$DEB"
