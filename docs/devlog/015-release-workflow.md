# Devlog 015 — The release workflow, and 1.0.0 made ready

**Date:** 2026-10-30

## What was built

The repository had been set to private by accident; that is why GitHub
Actions stopped (devlog 011 built everything locally instead). It is public
again, the workflows run, and 1.0.0 — prepared on 2026-10-24, never tagged —
gets released through a workflow of its own.

- **`.github/workflows/release.yml`.** A tag `vx.y.z` runs five jobs:
  `check` (version and release date everywhere, the tag names CMake's
  version, the notes can be made), `deb`, `flatpak`, `windows` in parallel,
  and `release`, which creates a **draft** release with the four packages,
  `SHA256SUMS` and the notes. Started by hand it is a dry run: the same
  builds, the packages as artifacts, no release.
- **`tools/release-notes.sh VERSION [SHA256SUMS]`** — the version's
  CHANGELOG section (relative links pointed at the tag), how to install, the
  sums.
- **`tools/check-version.sh` checks the date too**: the metainfo's
  `<release date>`, the Debian changelog's trailer and the man page's `.TH`
  must carry CHANGELOG.md's date. 1.0.0 was dated 2026-10-24 in four places;
  a release a week later would have shipped that date.
- **1.0.0's content.** What collected under `[Unreleased]` since (devlogs
  013, 014: uploads into an empty folder, a job's progress in the status
  bar) went into the 1.0.0 section — nothing had been published. Dated
  2026-10-30 everywhere at first, the library's 0.1.0 too (it ships with
  it); the tag came on 2026-11-18, and with the fix below the date moved
  there.
- `packaging/flatpak/build-in-docker.sh`: `QBUCKET_FLATPAK_LINT_ALLOWED`
  replaces the fixed list of tolerated lint errors.

## Decisions and why

- **The workflow runs the local scripts.** `packaging/debian/build-in-docker.sh`,
  `packaging/flatpak/build-in-docker.sh` and `packaging\windows\build-release.ps1
  -TestInstaller` are what devlogs 011 and 012 tested; the workflow adds
  nothing but runners, artifacts and the draft. One way to build a package,
  so a local build stays the fallback and reproduces a CI failure.
- **A draft, not a release.** The checks by hand (`docs/RELEASING.md` §5:
  the Windows setup for all users, KDE / GNOME, dragging out, the portals)
  need the very packages that will be published. The maintainer publishes.
- **Windows with Qt 6.10.2**, as `ci.yml`: aqtinstall can't install 6.11+
  yet. The machine that ran devlog 012 has 6.12; both pass the suite. The
  draft's setup is what the Windows check by hand installs.
- **Flatpak lint: what only Flathub can do is tolerated, nothing else.**
  With the repository public the homepage is reachable
  (`appid-url-not-reachable` gone). The metainfo's screenshots are files of
  the tag: a tag build must find them, a dry run can't
  (`appstream-missing-screenshots` tolerated there only). Found, they
  aren't on Flathub's media server (`appstream-external-screenshot-url`) nor
  mirrored into the repo (`appstream-screenshots-not-mirrored-in-ostree`) —
  both only Flathub's builders do; tolerated in every build. The local
  default tolerates all three.
- **Runner sizes.** `QBUCKET_JOBS=4`, `QBUCKET_DOCKER_MEMORY=12g` (4 cores,
  16 GB); the Flatpak job deletes preinstalled SDKs first (the builder image,
  the KDE SDK and the smoke test's runtime need the room). Inno Setup 6 is on
  `windows-2022`.
- **Flathub after 1.0.0** — the manifest in `packaging/flathub/` waits for
  the tag's commit.

## Checks

The three commits made on Windows (devlogs 013, 014) and this step, on
Linux:

- Debug ctest 102 / 102 with Ceph, VersityGW and Garage; release `-Werror`
  96 / 96; the app under ASan + UBSan 96 / 96 (the library unchanged since
  devlog 012's run).
- clang-tidy found one thing in a test from Windows:
  `test_main_window_operations` searched the message box's accept button
  into a pointer that the analyzer saw dereferenced on a null path after
  `QVERIFY`. Now `std::find_if` over the buttons; 240 files clean.
- `update_translations` on Linux: no new, unfinished or vanished entry —
  only the location lines that lupdate on Windows had moved, back to the
  Linux state.
- actionlint 1.7.12 (Docker): `release.yml` clean. It reports two matrix
  properties in `ci.yml` that the one-entry matrix no longer defines
  (`build_type`, `sanitize` — empty at run time, handled by the
  expressions) and shellcheck style notes; left as they are.
- `tools/release-notes.sh 1.0.0` against the CHANGELOG; a version without a
  section fails. `check-version.sh` with three wrong dates fails, naming each.
- The Flatpak built locally with the new lint list: 96 / 96 in the sandbox;
  the manifest without errors, the repo with only
  `appstream-missing-screenshots` and
  `appstream-screenshots-not-mirrored-in-ostree` — the homepage is reachable
  now; the smoke test's keychain round trip through the Secret portal.

## The dry run, and the first tag run

The dry run (2026-10-30) was green: the four packages, the Windows
`--self-test` with the Credential Manager on the runner, the installer test.

The tag `v1.0.0` (2026-11-18) failed in the Flatpak job's lint — .deb and
Windows green, no draft created: `appstream-external-screenshot-url`.
flatpak-builder-lint checks the screenshots one step at a time
(`checks/screenshots.py`) and returns at the first failure. Without the tag
the screenshot URLs 404, the catalogue has no screenshots, and the check
stops at `appstream-missing-screenshots` — so the dry run could never reach
the next step: every screenshot must start with Flathub's media URL (it
skips that only in Flathub's own pipeline). Tolerated now for the tag too;
a screenshot the tag build can't reach still fails it with
`appstream-missing-screenshots`. The tag had to move (RELEASING §4), so the
release date moved to the tag's day.
Checked locally before the new tag, with the tag build's list against the
published `v1.0.0`: the screenshots found, lint with only those two errors,
96 / 96 in the sandbox, the keychain through the Secret portal.

**A dry run can't show what only exists with the tag.** Here: the
screenshots. The metainfo's URLs at the tag are the one thing the dry run
couldn't check.

## Open points

- `ci.yml`'s Windows ZIP is still the old fixed copy (no MSVC runtime); the
  release ZIP comes from `build-release.ps1`.
- Flathub; code signing (SmartScreen).
