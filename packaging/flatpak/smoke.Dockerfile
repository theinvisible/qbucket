# Runs the QBucket Flatpak inside a container, as on a desktop without a
# display: flatpak, a D-Bus session, xdg-desktop-portal and GNOME Keyring as
# the Secret portal's backend. Used by build-in-docker.sh (smoke.sh).
FROM fedora:44
RUN dnf install -y --setopt=install_weak_deps=False \
        flatpak dbus-daemon dbus-tools xdg-desktop-portal gnome-keyring procps-ng \
 && dnf clean all
