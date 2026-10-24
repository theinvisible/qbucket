# Releasing QBucket

How a release is checked, built and published. GitHub Actions don't run for
this repository at the moment (no build minutes), so everything the
workflows did happens locally — Linux on the development machine and in
Docker, Windows on a Windows machine. The workflows stay in `.github/` and
work again once Actions do.

## 1. Version

One version, set in `project(qbucket VERSION x.y.z)` in `CMakeLists.txt`.
The same number goes into:

- `CHANGELOG.md` — a new `## [x.y.z] — date` section on top;
- `packaging/linux/io.github.theinvisible.QBucket.metainfo.xml` — a new
  `<release version="x.y.z" date="…">` first in `<releases>`;
- `packaging/debian/changelog` — a new entry `qbucket (x.y.z-1) resolute`;
- `packaging/debian/qbucket.1` — the `.TH` line;
- `packaging/flathub/io.github.theinvisible.QBucket.yml` — `tag` / `commit`.

`tools/check-version.sh` (ctest `version_consistency`) fails while the
first four disagree with CMake. The Windows resource and the installer take
the version from CMake. The library `libs/qbucket-s3` has its own version and
`CHANGELOG.md`; bump it only when its API changed.

## 2. Checks (instead of CI)

On the Linux machine (Ubuntu 26.04 with the distribution's Qt), one after
the other, at `-j12`:

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
```

When the protocol changed: the Hetzner run (CLAUDE.md, "Build & test"), with
the guard objects checked before and after.

## 3. Packages

```sh
packaging/debian/build-in-docker.sh     # dist/qbucket_x.y.z-1_amd64.deb
packaging/flatpak/build-in-docker.sh    # dist/qbucket-x.y.z-x86_64.flatpak
```

Each builds with the whole test suite inside, then installs the result in a
clean container and runs `qbucket --version`, the window (offscreen) and
`qbucket --self-test` — the deb without a keychain (must fall back to
session-only secrets) and with GNOME Keyring, the Flatpak through the Secret
portal. Lintian (deb) and flatpak-builder-lint (Flatpak) must have no
errors; the Flatpak's three expected ones are those of a private repository
(`packaging/flathub/README.md`). Docker is all they need on the host.

On Windows (Visual Studio 2022 with C++, Qt `msvc2022_64`, Inno Setup 6):

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\build-release.ps1
# dist\qbucket-x.y.z-windows-x64.zip, dist\qbucket-x.y.z-windows-x64-setup.exe
```

It builds with `/WX`, runs ctest (including `test_virtual_file_drag`),
stages the windeployqt folder, checks the staged `qbucket.exe` (`--version`,
`--self-test` against the Credential Manager — not possible in an SSH
session with key login), then zips and builds the installer.

## 4. By hand

Not automated; each release on the real desktops:

- **Windows** — install with the setup (per user), start from the Start menu,
  add an account (secret in the Credential Manager), browse, upload and
  download; drag a file, several files and a folder into Explorer, a large
  file (Explorer's progress, QBucket stays usable), Escape and Explorer's
  Cancel; uninstall (settings and secrets stay); an update over an older
  version.
- **Ubuntu 26.04 (.deb)** — `sudo apt install ./qbucket_x.y.z-1_amd64.deb`;
  KDE and GNOME: an account with the secret in KWallet / GNOME Keyring, drag
  into Dolphin / Nautilus, open in the default application and upload the
  change.
- **Flatpak** — `flatpak install --user qbucket-x.y.z-x86_64.flatpak` on KDE
  and GNOME: the secret through the Secret portal, drag out, open through the
  OpenURI portal (writable for the other application?), drag in from the
  host, import through the file chooser.

## 5. Publish

The maintainer tags and publishes (nothing is pushed from automation):

```sh
git tag -a vx.y.z -m "QBucket x.y.z"
git push origin main vx.y.z
gh release create vx.y.z --title "QBucket x.y.z" --notes-file <notes> \
    dist/qbucket_x.y.z-1_amd64.deb dist/qbucket-x.y.z-x86_64.flatpak \
    dist/qbucket-x.y.z-windows-x64.zip dist/qbucket-x.y.z-windows-x64-setup.exe
```

The notes: the version's section of `CHANGELOG.md`, plus the SHA-256 sums
(`sha256sum dist/*`; the Windows script prints its own).
