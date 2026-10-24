#!/usr/bin/env bash
#
# Inside smoke.Dockerfile's container (privileged, for bubblewrap):
#   smoke.sh BUNDLE APP_ID RUNTIME_REF
# Installs the runtime from Flathub (cached in /var/lib/flatpak) and the
# bundle, then runs the app in its sandbox:
#   --version; the window for 5 s (offscreen); --self-test with the Secret
#   portal served by xdg-desktop-portal + GNOME Keyring on a D-Bus session.
set -euo pipefail
BUNDLE=$1 APP_ID=$2 RUNTIME=$3
log() { printf '\n--- %s\n' "$*"; }

flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
log "runtime $RUNTIME"
flatpak install -y --noninteractive flathub "$RUNTIME" >/dev/null
flatpak install -y --noninteractive --reinstall "$BUNDLE" >/dev/null
flatpak info "$APP_ID" | sed -n '/Version\|Runtime\|Commit/p'

# A desktop session, minus the display.
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export XDG_CURRENT_DESKTOP=GNOME
# xdg-desktop-portal: GNOME Keyring implements the Secret portal.
mkdir -p /etc/xdg/xdg-desktop-portal
printf '[preferred]\ndefault=none\norg.freedesktop.impl.portal.Secret=gnome-keyring\n' \
    > /etc/xdg/xdg-desktop-portal/portals.conf

exec dbus-run-session -- bash -euc '
    APP_ID='"$APP_ID"'
    printf test | gnome-keyring-daemon --unlock --components=secrets >/dev/null
    /usr/libexec/xdg-desktop-portal >/tmp/portal.log 2>&1 &
    sleep 2
    run() { flatpak run --env=QT_QPA_PLATFORM=offscreen "$APP_ID" "$@"; }

    echo; echo "--- qbucket --version"
    run --version

    echo; echo "--- the window starts (offscreen)"
    rc=0; timeout 8 flatpak run --env=QT_QPA_PLATFORM=offscreen "$APP_ID" || rc=$?
    [ $rc -eq 124 ] || { echo "qbucket exited with $rc before the timeout"; exit 1; }

    echo; echo "--- self-test in the sandbox"
    rc=0; run --self-test || rc=$?
    if [ $rc -ne 0 ]; then
        echo "self-test failed ($rc); portal log:"; tail -20 /tmp/portal.log
        exit 1
    fi
    # Through the Secret portal libsecret keeps an encrypted keyring file in
    # the app'"'"'s own data, with the key from the portal (GNOME Keyring here).
    keyring="$HOME/.var/app/$APP_ID/data/keyrings"
    if ls "$keyring"/*.keyring >/dev/null 2>&1; then
        echo "secrets: via the Secret portal ($keyring)"
    else
        echo "secrets: not through the portal (no $keyring)"; exit 1
    fi'
