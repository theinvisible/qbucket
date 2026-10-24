# Devlog 011 — M8: Packaging & release 1.0.0

**Date:** 2026-10-24

## What was built

The last milestone of PLAN.md §6: from the code to installable packages and
a first release. Two things changed the plan before it started. GitHub
Actions stopped running for this repository (no build minutes left): the CI
was going to build the release artifacts, so now everything runs locally —
nothing installed on the host, everything that needs a toolchain or a
runtime runs in Docker. And the repository is private, so no Flathub
submission yet. Agreed: version **1.0.0**; a Flatpak bundle; a **.deb for
Ubuntu 26.04** (new); the Windows installer prepared here and built and
tested by the maintainer on Windows.

| Stage | Commit | What |
|---|---|---|
| R1 | `b6f833d` | Version 1.0.0, `--version` / `--help` / `--self-test`, CHANGELOG.md, VERSIONINFO, `version_consistency`, clang-tidy in Docker |
| fix | `ef1e48d` | No keychain daemon on Linux → session-only secrets, not a failed save |
| R2 | `0d37bc1` | `.deb` for Ubuntu 26.04, built and smoke-tested in Docker; the release screenshot test |
| R3 | `6e2e8b6` | Flatpak bundle built, linted and smoke-tested in Docker; AppStream for 1.0.0; the Flathub manifest |
| R4 | `ac95262` | Windows: `build-release.ps1` and the Inno Setup installer (prepared) |
| R5 | this entry | Screenshots, README, `docs/RELEASING.md`, docs |

![QBucket 1.0.0](../img/main-dark.png)

## Decisions and why

- **`qbucket --self-test` is every package's smoke test.** Without a window,
  account or network: version, Qt at runtime, sandbox, paths, and a round
  trip through the keychain with an entry of its own (written, read back,
  removed; a fresh value each time, so a stale entry can't pass). Exit 0 or
  1. It runs on `QCoreApplication`, so it needs no display; on Windows a GUI
  executable attaches to the parent's console first
  (`platform/Console`). The same command is what a bug report asks for.
- **Packages build with the test suite inside.** The .deb runs ctest in
  `dh_auto_test`, the Flatpak with `run-tests: true` in the build sandbox
  (offscreen, fake servers on loopback) — what ships is what was tested,
  against the Qt it ships with (Ubuntu's 6.10.2, the KDE runtime's 6.10.3).
- **Then a clean install.** `packaging/debian/build-in-docker.sh` installs
  the .deb in a second, fresh `ubuntu:26.04` (its Depends must suffice) and
  runs `--version`, the window offscreen for 5 s, the self-test without a
  session bus, with a bus that has no Secret Service and with an unlocked
  GNOME Keyring, then `desktop-file-validate` and `appstreamcli`.
  `packaging/flatpak/build-in-docker.sh` builds in Flathub's own builder
  image and runs the bundle in a Fedora container with xdg-desktop-portal
  and GNOME Keyring as the Secret portal's backend.
- **The Flatpak keeps its finish-args.** The open question of PLAN.md §9:
  does QtKeychain → libsecret work through the Secret portal, or does the
  app need `--talk-name=org.freedesktop.secrets`? The smoke test answers it:
  the round trip works with the portal, and libsecret's encrypted keyring
  file appears in the app's own data — proof that the portal path was taken.
  KDE (KWallet's portal) remains a check by hand.
- **lint strict, with three known exceptions.** flatpak-builder-lint errors
  fail the build, except `appid-url-not-reachable`,
  `appstream-missing-screenshots` and
  `appstream-screenshots-not-mirrored-in-ostree`: a private repository's
  URLs are unreachable, and Flathub mirrors screenshots on its own builders.
- **One version.** `project(qbucket VERSION 1.0.0)`; `tools/check-version.sh`
  (ctest) holds CHANGELOG.md, the metainfo, the Debian changelog and the man
  page to it; the Windows resource and installer take it from CMake. The
  library stays 0.1.0 (now dated): its API isn't frozen.
- **No AppImage, no static binary** (PLAN.md §10).
- **Windows prepared, not run.** `build-release.ps1` does what the Windows CI
  job did (vswhere → MSVC, `/WX`, ctest, windeployqt staging, smoke test of
  the staged exe, ZIP, ISCC, SHA-256). Here it was only parsed by PowerShell
  in Docker, and the `.iss` compiled by ISCC under Wine with a dummy stage.
  Per-user install by default, "for all users" offered; uninstalling keeps
  settings and secrets.
- **clang-tidy in Docker.** The pip clang-tidy lived in a scratch directory
  that a reboot emptied; `tools/tidy-in-docker.sh` runs the same scripts from
  an Ubuntu 26.04 image with Qt 6.10, mounting the repository at its own
  path, so the host's `compile_commands.json` works as it is.

![The light theme](../img/main-light.png)

## The keychain bug the .deb found

The first smoke test in a clean Ubuntu said `Keychain: write: failed
(Unknown error)` without a keyring — where the app's design (and devlog 003)
promise session-only secrets. QtKeychain on Linux never reports
`NoBackendAvailable` for a missing daemon: with libsecret it passes on the
D-Bus error as `OtherError`; without libsecret it falls back to the GNOME
Keyring backend, whose library isn't there, and answers "Unknown error".
`SecretStore` saw a failure, and **an account could not be saved at all** on
a desktop without a Secret Service.

`QtKeychainBackend` now settles every failure with
`platform::checkKeychainService` (asynchronous): the Secret Service or
KWallet on the session bus, running or activatable, and in a Flatpak the
Secret portal; no session bus means none. Nothing there → `NoBackend` →
session-only. A real keychain that fails stays a failure.
`test_keychain_service` runs the real QtKeychain against a private
`dbus-daemon` with a configuration of its own (the system's service
directories would offer this machine's keyring for activation), and finds a
fake Secret Service once one is activatable or running.

## Measurements

- ctest 102 / 102 (debug, with the Docker backends); release with `-Werror`
  96 / 96 (without Docker); ASan for the new tests; clang-tidy over 236
  files clean (R1), the files changed since clean as well.
- .deb: 95 / 95 tests during the package build, lintian without a tag
  (after a man page and an override for `initial-upload-closes-no-bugs`),
  installed and smoke-tested as above.
- Flatpak: 96 / 96 tests in the build sandbox, lint with only the three known
  errors, smoke test green with the keychain through the portal.

## Pitfalls

- **flatpak-builder wants `--state-dir` on the build directory's file
  system** — with the state in a Docker volume and the build in the
  container's overlay it refuses to start. Everything lives in the volume.
- **A D-Bus config without receive rules** (`<allow send_destination="*"/>`
  only) rejects the replies to the app's own calls; a check then "finds
  nothing" for the wrong reason. Allow eavesdrop / own as the test's config
  does, and look at the daemon's log.
- **`dbus-daemon --session` reads the system's service directories**: on a
  desktop with GNOME Keyring installed, `org.freedesktop.secrets` is
  activatable even on a private bus. Tests need their own `--config-file`.
- **Inno Setup and rc.exe read files without a BOM in the ANSI code page**:
  `installer.iss` and `app.rc.in` stay ASCII.
- **`QTest::qWaitFor` is `[[nodiscard]]`**: with `-Werror` an ignored result
  breaks the build.
- The old screenshots showed a Hetzner guard bucket and an empty window;
  `test_release_screenshots` builds the pictures from a FakeObjectStore with
  believable content, English formats whatever the machine's locale.

## Still to check — by hand

- **Windows**: `build-release.ps1` on a real machine (MSVC `/WX`,
  `test_virtual_file_drag`), installer per user and for all users, update,
  uninstall; the Explorer drag from devlog 010.
- **KDE**: the Flatpak's secrets through KWallet's Secret portal; the .deb
  with KWallet; dragging into Dolphin.
- **GNOME**: Nautilus drag, the OpenURI portal (is the opened copy writable?).

## Open points

- Flathub: needs a public repository (`packaging/flathub/README.md`).
- The KDE runtime 6.11 is out; moving needs a full Flatpak run.
- AWS S3 itself has never been run against (README provider table).
- GitHub Actions: the workflows are unchanged and run again with minutes —
  or on a public repository.
