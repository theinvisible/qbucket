# Releasing QBucket

How a release is checked, built and published. A tag `vx.y.z` starts
`.github/workflows/release.yml`: it builds the four packages with the same
scripts a local release uses, each with the whole test suite in the build
and a smoke test of the installed result, and creates a **draft** release.
The maintainer checks the draft's packages by hand and publishes it. Nothing
is pushed, tagged or published by automation on the maintainer's machine.

## 1. Version and date

One version, set in `project(qbucket VERSION x.y.z)` in `CMakeLists.txt`.
The same number and the release date go into:

- `CHANGELOG.md` — a new `## [x.y.z] — YYYY-MM-DD` section on top (what
  collected under `## [Unreleased]` moves there);
- `packaging/linux/io.github.theinvisible.QBucket.metainfo.xml` — a new
  `<release version="x.y.z" date="YYYY-MM-DD">` first in `<releases>`;
- `packaging/debian/changelog` — a new entry `qbucket (x.y.z-1) resolute`;
- `packaging/debian/qbucket.1` — the `.TH` line;
- `packaging/flathub/io.github.theinvisible.QBucket.yml` — `tag` / `commit`
  (once QBucket is on Flathub, `packaging/flathub/README.md`).

`tools/check-version.sh` (ctest `version_consistency`, and the release
workflow's first job) fails while the first four disagree with CMake on the
version or with `CHANGELOG.md` on the date — a release a day later means
moving the date everywhere. The Windows resource and the installer take the
version from CMake. The library `libs/qbucket-s3` has its own version and
`CHANGELOG.md`; bump it only when its API changed.

## 2. Checks

CI (`ci.yml`) runs on every push: Linux Release, the Windows app, the library
as a Windows DLL. It must be green on the commit to tag. Start
`ci-extended.yml` (sanitizers, clang-tidy, Flatpak, library variants) and,
when the protocol changed, `s3-integration.yml` by hand from the Actions tab.

Every commit is checked locally first as well. On the Linux machine (Ubuntu
26.04 with the distribution's Qt), one after the other, at `-j12`:

```sh
# debug build, every test; Docker backends up for the docker label
libs/qbucket-s3/tests/docker/it-up.sh
cmake --build build -j12 && ctest --test-dir build -j12 --output-on-failure
# release build with -Werror
cmake --build build-release -j12 && ctest --test-dir build-release -LE docker -j12
# the library alone under ASan + UBSan + LSan
cmake --build build-s3-asan -j12 && ctest --test-dir build-s3-asan
# the app under ASan + UBSan
cmake --build build-asan -j12 && ctest --test-dir build-asan -LE docker
# clang-tidy 18 (Docker; ~10 min)
tools/tidy-in-docker.sh run-clang-tidy.sh build
# the translations: only location lines may move, nothing unfinished
cmake --build build --target update_translations
```

When the protocol changed: the Hetzner run (CLAUDE.md, "Build & test"), with
the guard objects checked before and after.

## 3. Dry run

Before the tag, start **Release** from the Actions tab (or
`gh workflow run release.yml --ref main`). It builds and tests everything a
tag would, and keeps the packages as the run's artifacts — no release. The
only difference: the metainfo's screenshots are files of the tag, so its
Flatpak lint lets `appstream-missing-screenshots` pass.

## 4. Tag → draft release

```sh
git tag -a vx.y.z -m "QBucket x.y.z"
git push origin vx.y.z
```

`release.yml` then

- checks the version and date everywhere, and that the tag names CMake's
  version;
- **.deb** — `packaging/debian/build-in-docker.sh`: built in `ubuntu:26.04`
  with the tests and lintian, installed in a clean container: `--version`,
  the window (offscreen), `--self-test` without a keychain (must fall back
  to session-only secrets) and with GNOME Keyring, desktop file and
  metainfo;
- **Flatpak** — `packaging/flatpak/build-in-docker.sh`: Flathub's builder
  image, the tests in the build sandbox, flatpak-builder-lint (only
  `appstream-screenshots-not-mirrored-in-ostree` may pass — Flathub mirrors
  screenshots on its own builders), the bundle run in Fedora with the
  keychain through the Secret portal;
- **Windows** — `packaging\windows\build-release.ps1 -TestInstaller` on
  `windows-2022` with Qt 6.10.2 (as `ci.yml`): `/WX`, ctest, windeployqt
  into an empty folder with the MSVC runtime app-local, the `dumpbin`
  dependency check, the staged exe's `--version` and `--self-test`, ZIP,
  Inno Setup installer, then `test-installer.ps1` (an older version per user,
  the update, the uninstall);
- creates the **draft** `vx.y.z` with the four packages, `SHA256SUMS` and the
  notes of `tools/release-notes.sh` (the version's CHANGELOG section, how to
  install, the sums).

A failed run: fix, commit, push, then move the tag
(`git tag -d vx.y.z && git push origin :refs/tags/vx.y.z`, tag again) and
delete a draft that was already created.

## 5. By hand

Not automated; each release on the real desktops, with the draft's packages:

- **Windows** — install with the setup (per user, and once "for all users"),
  start from the Start menu, add an account (secret in the Credential
  Manager), browse, upload and download; drag a file, several files and a
  folder into Explorer, a large file (Explorer's progress, QBucket stays
  usable), Escape and Explorer's Cancel; uninstall (settings and secrets
  stay); an update over an older version.
- **Ubuntu 26.04 (.deb)** — `sudo apt install ./qbucket_x.y.z-1_amd64.deb`;
  KDE and GNOME: an account with the secret in KWallet / GNOME Keyring, drag
  into Dolphin / Nautilus, open in the default application and upload the
  change.
- **Flatpak** — `flatpak install --user qbucket-x.y.z-x86_64.flatpak` on KDE
  and GNOME: the secret through the Secret portal, drag out, open through the
  OpenURI portal (writable for the other application?), drag in from the
  host, import through the file chooser.

Then publish the draft (GitHub, or `gh release edit vx.y.z --draft=false`).

## 6. Without the workflow

The same packages locally, should Actions be unavailable — Docker is all the
Linux ones need on the host; Windows needs Visual Studio 2022 with C++, Qt
`msvc2022_64` and Inno Setup 6:

```sh
packaging/debian/build-in-docker.sh     # dist/qbucket_x.y.z-1_amd64.deb
packaging/flatpak/build-in-docker.sh    # dist/qbucket-x.y.z-x86_64.flatpak
```

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\build-release.ps1 -TestInstaller
# dist\qbucket-x.y.z-windows-x64.zip, dist\qbucket-x.y.z-windows-x64-setup.exe
```

The Linux scripts compile with 12 jobs in a container limited to 20 GB
(`QBUCKET_JOBS`, `QBUCKET_DOCKER_MEMORY`; with every core the machine can
hang); `QBUCKET_FLATPAK_LINT_ALLOWED` lists the lint errors that may pass.
`--self-test` on Windows needs an interactive or password logon — not an SSH
session with key login. Then:

```sh
(cd dist && sha256sum -- *.deb *.flatpak *.zip *-setup.exe > SHA256SUMS)
tools/release-notes.sh x.y.z dist/SHA256SUMS > notes.md
gh release create vx.y.z --draft --verify-tag --title "QBucket x.y.z" \
    --notes-file notes.md dist/SHA256SUMS \
    dist/qbucket_x.y.z-1_amd64.deb dist/qbucket-x.y.z-x86_64.flatpak \
    dist/qbucket-x.y.z-windows-x64.zip dist/qbucket-x.y.z-windows-x64-setup.exe
```
