# QBucket

A Qt6/C++ desktop browser for S3 and S3-compatible object storage (AWS,
Hetzner Object Storage, Ceph RGW, MinIO, Cloudflare R2, Backblaze B2, …).
Feature yardstick: NetSDK's **S3 Browser** — but open source and
cross-platform.

Targets: **Windows** and **Linux** as equals; Linux ships as a **Flatpak**
first. Stay platform-neutral outside `src/platform/`.

License: **MIT**.

Provider priority: **Hetzner Object Storage first.** Everything
provider-specific goes through a provider profile in the library
(`libs/qbucket-s3/include/qbucket/s3/providers/`) — see "Provider profiles"
below.

## Repository split: library + app

- **`libs/qbucket-s3/`** — reusable S3 library, meant for use in other
  projects too (`PLAN.md` §3.0). Targets `qbucket::s3`,
  `qbucket::s3_transfer`, `qbucket::s3_testsupport`; namespace
  `qbucket::s3`; own semver + `CHANGELOG.md`.
- **`src/`** — the QBucket app. Uses **only** the library's public headers
  (`#include <qbucket/s3/...>`).

Library rules (a change that breaks one of these is wrong, however
convenient):

- Depends on Qt6 Core / Network / Concurrent only — no Widgets, no
  QtKeychain, no `QSettings`, no app types, no hard-coded paths.
- Never includes anything from `src/`. Public API = `include/qbucket/s3/**`;
  everything else is private. Public `QObject` classes use PIMPL.
- App knowledge enters only through interfaces: `CredentialsProvider`,
  `RequestObserver`, `ResumeStore`, logging categories.
- Protocol-level safety (Content-MD5, 200-with-error, idempotent retries,
  listing loop detection, redaction, integrity checks) lives in the
  library; policy (read-only, confirmations, plan/preview, environments)
  lives in the app.
- Thread-safety is documented per public class.
- The library must build and pass its tests standalone
  (`cmake -S libs/qbucket-s3 -B build-s3`); a CI job enforces it.
- API changes go into the library's `CHANGELOG.md`.

## Status

**M0 (skeleton) done** — devlog 001; verified on Linux in devlog 002.
**M1 stages 1–14 done** — devlog 003; stage 15 (Hetzner validation) open.
The full plan — decisions, scope, architecture, milestones M0–M8, test
strategy, protocol pitfalls — lives in **`PLAN.md`**. Start there, and
update this section as milestones land.

What exists:

- App skeleton (M0): CMake with Qt auto-detect and `qbucket::compile_options`,
  design system (`ui/Theme`, `ui/DarkFusionStyle`, `ui/IconFactory`,
  `ui/ThemeManager`), `MainWindow` with the full §4 layout on placeholder
  models (every mutating action disabled), `SettingsDialog`,
  `SettingsController`, `diagnostics/StallWatchdog`, German translation,
  packaging (icon, `.desktop`, AppStream, Flatpak).
- **`libs/qbucket-s3`** (M1): `Session` (worker thread, callback + context,
  cancel/shutdown), SigV4 + `Presigner`, read/write object and bucket
  operations, safe retries, clock skew, region redirects,
  `ListObjectsPager`, provider registry (Hetzner, AWS, Garage, generic),
  capability safeguard (probes, `CapabilitySet`, short-circuit, learning),
  `testing::FakeS3Server`, `examples/s3ls`. See its README and CHANGELOG.
- Docker test backends: `libs/qbucket-s3/tests/docker/it-up.sh` (Ceph RGW,
  VersityGW, Garage), integration test `it_s3_backend`
  (`-DQBUCKET_S3_IT_LOCAL=ON`, `ctest -L docker`), known backend deviations in
  `tests/data/it-expectations/`.
- App accounts (M1): `accounts/SecretStore` (QtKeychain, session-only mode),
  `accounts/AccountStore`, `accounts/KeychainCredentialsProvider`,
  `controllers/ConnectionTester`, `ui/AccountDialog`.
- CI: Windows, Linux, ASan/UBSan, clang-tidy, Flatpak, `s3-standalone`
  (static/shared/Windows/ASan+LSan), `s3-integration.yml` (Docker backends).

Next: **M1 stage 15** — Hetzner integration run (one region), fixtures,
`docs/providers/hetzner.md`; then **M2** (browser UI).

## Devlog

**[docs/devlog/](docs/devlog/)** — one entry per step, numbered
(`NNN-short-slug.md`), **written in English**, format as in
`000-project-start-and-planning.md` (`# Devlog NNN — Title`, `**Date:**`,
then: what was built, decisions + why, measurements, pitfalls, open
points). Add a new entry after every completed milestone and after any
notable detour (bug hunt, design change, provider quirk discovered).
Screenshots go to `docs/devlog/img/`.

## Stack

- **Qt 6.5+** (local: `H:/Qt/6.12.0/msvc2022_64`), C++20, CMake ≥ 3.22
- **UI: Qt Widgets** on Fusion with Dark / Light / System theming — design
  system ported from the sibling project **xyz-profiler**
  (`H:\DEV\CLion\xyz-profiler`: `ui/Theme`, `ui/DarkFusionStyle`,
  `ui/IconFactory`). Keep the palette tokens identical.
- **S3:** own client on `QNetworkAccessManager` (SigV4 + `QXmlStreamReader`).
  No AWS SDK.
- **Secrets:** every password / secret key is stored with **QtKeychain**
  (`cmake/QBucketQtKeychain.cmake`: installed package if it matches our Qt,
  else FetchContent 0.15.0 static; Flatpak builds it as a manifest module)
  through `accounts/SecretStore` — never in the INI, a JSON
  file, logs or the transfer resume store. No plaintext fallback: without a
  keychain backend the account asks once per session and keeps the secret
  in memory only.
- **Settings:** `QSettings` INI through `SettingsController`.

## Build & test

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target qbucket -j
ctest --test-dir build --output-on-failure
```

The top-level `CMakeLists.txt` auto-detects Qt under `H:/Qt` / `C:/Qt` (same
logic as xyz-profiler). Override with `-DCMAKE_PREFIX_PATH=...`.

Test binaries land in `build/tests/` (not next to `qbucket.exe`, see the
lessons below) and run on the `offscreen` platform. To refresh the devlog
screenshots, run the window test on a real platform:
`QT_QPA_PLATFORM=windows QBUCKET_SCREENSHOT_DIR=docs/devlog/img build/tests/test_main_window`
(`xcb` / `wayland` on Linux).

Integration tests against a real endpoint are opt-in via `QBUCKET_IT_*`
environment variables (endpoint, region, access key, secret, bucket,
addressing style) and are skipped when unset.

## Conventions

Inherited from xyz-profiler — keep both projects consistent.

- `namespace qbucket`. **No `Q` class prefix** — that reads as Qt API.
- Layering: library `qbucket-s3` ← app `domain` ← `accounts` ←
  `controllers` / `models` ← `ui`. Nothing below `models` includes widget
  headers.
- Business logic in `QObject` controllers; the UI connects to signals and
  calls public methods. Dialogs are `QDialog` subclasses.
- **The UI never blocks, on any action or data size.** The GUI thread only
  paints, handles widgets and appends finished batches to models.
  `S3Client` + its own QNAM live on worker threads (browse thread per
  account, transfer pool); the GUI talks to them only via the library's
  `s3::Session` proxy (queued calls, value-type results, `RequestId` cancellation).
  Hashing, directory walks, plan/journal I/O and large sorts run in the I/O
  pool. Never share a QNAM across threads; workers never touch widgets or
  models. No `QDialog::exec()` around long work, no `QEventLoop::exec()`,
  no `waitForFinished()` on the GUI thread. See `PLAN.md` §3 threading
  model.
- C++20, no exceptions on hot paths; errors flow back as result structs /
  `S3Error` or via signals.
- `QStringLiteral` / `u"…"` for literals; no implicit `const char*` →
  `QString`.
- Headers under `src/<module>/`, included as `"module/Header.h"`. One class
  per file, filename = class name.
- Tests: QtTest, one binary per module, `QTEST_GUILESS_MAIN` for non-UI
  tests, fixtures via `TEST_DATA_DIR`. Library tests + fixtures in
  `libs/qbucket-s3/tests/`, app tests in `tests/`.
- New UI strings: run `lupdate` and add the German entry to
  `translations/qbucket_de.ts`.
- Commits: Conventional Commits. Early phase: directly on `main` (one
  commit per stage, ctest green); feature branches `feat/<slug>` later.

## Lessons carried over from xyz-profiler

Each of these cost time there; don't relearn them.

- **Settings writes are synchronous disk I/O.** Debounce anything
  high-frequency (splitter, column widths, window geometry) with a 300 ms
  single-shot timer, flushed on `aboutToQuit`.
- **No catch-all `QWidget {}` QSS rule**, install Fusion only once, set the
  font only when it changed — otherwise every theme switch re-polishes the
  whole tree.
- **`QHeaderView::ResizeToContents` is O(rows)** per resize. Use
  `Interactive` with fixed widths for predictable columns — critical with
  100k-row object listings.
- **Debug builds are 8–13× slower** in the paint stack (debug Qt DLLs + CRT).
  Check the build profile before chasing a performance complaint.
- **Standard shortcuts on Windows:** route `QKeySequence::Quit` /
  `Preferences` through a `setStandardShortcut()` helper, otherwise menus
  show phantom "Exit"/"Settings" text.
- **Column indices persisted in settings are append-only.** Never insert a
  column enum value anywhere but at the end.
- **Delegates never do I/O in `paint()`.** Anything that needs loading
  (thumbnails, lazy HEADs) goes async and triggers a repaint.

## Lessons from QBucket itself

- **`QApplication::style()->name()` is empty once a stylesheet is set** —
  `style()` then returns Qt's `QStyleSheetStyle` proxy. The xyz-profiler
  "install Fusion only once" check relied on the name and therefore
  re-installed Fusion (full re-polish) on every theme switch.
  `DarkFusionStyle` now remembers it in a static flag; the regression test
  `themeSwitchKeepsTheStyleObject` guards it. (xyz-profiler likely still
  has the bug.)
- **Test executables must not share the app's output directory on
  Windows.** windeployqt copies a trimmed Qt (only the `windows` platform
  plugin) next to `qbucket.exe`; a test there can't load `offscreen` and
  Qt aborts with a *modal* debug dialog. Tests build into `build/tests/`
  and get Qt's `bin` via `ENVIRONMENT_MODIFICATION` (a plain `PATH=a;b` in
  `ENVIRONMENT` is split as a CMake list).
- **A `&` in a tab or button label is a mnemonic** — "Headers & Meta"
  rendered as "Headers _Meta". Use `&&` or avoid the character.
- **The tab-bar base ignores the palette.** With `setDocumentMode(true)`
  the style paints the tab bar's base line in a fixed `#393e43` in both
  themes, and `QTabWidget::pane { border-top }` has no effect. The QSS
  sets `QTabBar { qproperty-drawBase: 0; border-bottom: … @border@ }`;
  test `tabBarsUseThemedSeparator` guards it. (Same in xyz-profiler.)

## Production data — safety rules

QBucket almost always runs against **production data**. Robustness beats
convenience and speed: **check once too often rather than once too
little.** Full rules in `PLAN.md` §12; the ones every change must respect:

- **One mutation gate:** every write/delete/copy/config change goes through
  `OperationController` (read-only accounts, environment marker,
  confirmation, journal, verification). The UI never calls the library's
  mutating API directly.
- **Dynamic capability safeguard** (`PLAN.md` §3.3): features are probed
  read-only on connect / bucket open; an `Unsupported` feature makes the
  action impossible (library short-circuit, `OperationController`
  `requires()`, UI with reason). `Unknown` never unlocks a destructive
  operation; a 5xx/timeout never flips a feature to `Unsupported`.
  Results expire and are re-probed automatically (server 24 h, bucket
  state 10 min + before destructive ops, immediately on a server
  fingerprint change); downgrades apply at once, upgrades after a
  validated answer, every change is shown to the user. **A new
  feature is only done with its `Feature` entry, probe, `requires()` and
  tests against a supporting backend and Garage.**
- **Plan → preview → execute → verify → report** for every bulk or
  destructive operation. Scope comes from a fresh listing, never from what
  the view shows.
- **Move = copy → verify → delete source.** Metadata edits merge the full
  current state and copy with `x-amz-copy-source-if-match`.
- **Never trust a status code alone:** CopyObject / UploadPartCopy /
  CompleteMultipartUpload can return 200 with an `<Error>` body.
- **Retry only idempotent requests**; resolve an unclear
  CompleteMultipartUpload by HEAD, not by retrying.
- **Integrity:** `Content-MD5` on every PUT and part; downloads verified and
  only then renamed from `.qbpart`. Report "size verified only" honestly.
- **An error is never an empty folder.** Listing loops (repeated
  continuation token) abort with an error.
- **No secrets in logs:** redact `Authorization`, presigned signatures,
  session tokens.
- Object-lock governance bypass only per operation with explicit
  confirmation; never implicit.

## Performance rules

Second priority after safety, but mandatory with millions of objects
(`PLAN.md` §13, budgets in §13.4, measured in release builds only):

- Never load a whole bucket; page with `fetchMore`, prefetch near the end.
- Compact row structs, lazy display strings, one `beginInsertRows` per
  page, no `dynamicSortFilter` re-sorting on appends.
- Bulk operations stream (list page → plan file → batch → next page);
  memory stays bounded at any scale.
- Adaptive concurrency (AIMD on SlowDown/503) per account.
- GUI-thread stalls > 50 ms are bugs; the debug stall watchdog reports
  them.

## Provider profiles

- **The protocol core never checks which provider it talks to.** No
  endpoint/hostname sniffing in the library's client/transfer code or
  anywhere in the app.
  Every deviation from plain S3 is a `ProviderProfile` hook or a
  `Capabilities` flag (`PLAN.md` §3.1).
- `GenericS3Profile` holds the S3 defaults; concrete profiles override only
  what differs. `HetznerProfile` is first-class; others are added as
  needed.
- Profile capabilities are only the fallback; definitive results of the
  capability prober win (§3.3).
- A new provider = one profile class + fixtures under
  `libs/qbucket-s3/tests/data/<provider>/` + `test_provider_<id>.cpp` + one
  registry line.
- Hetzner is the primary integration-test target; a change touching the
  protocol must pass the Hetzner integration run.

## S3 subsystem notes

Fill in as they're discovered. Seeded from `PLAN.md` §8:

- Send `Content-MD5` where S3 requires integrity headers; do not adopt the
  `x-amz-checksum-*` defaults (they break third-party providers).
- Path-style vs. virtual-hosted is per provider preset, overridable per
  account. Dotted bucket names need path-style over TLS.
- ETags are quoted and only equal the MD5 for single-part, non-KMS objects.
- "Folders" are prefixes; a `prefix/` marker object may or may not exist.
- Clock skew: correct from the server `Date` header, never ask the user to
  fix their clock.
- Self-signed TLS: per-account certificate pinning, never a global ignore.
- **QNAM normalises `.` / `..` path segments on the wire** — even
  `%2E%2E`, and although `QUrl::path()` still shows them. A key
  `a/../b` would address `b`. `S3Uri::build` refuses such keys
  (`InvalidRequest`); never work around it by sending them anyway.
- **No `deleteLater()` while a worker thread tears down.** Objects
  destroyed in a thread's final deferred-delete pass (after
  `QThread::finished`) must `delete` their own-thread children directly; a
  `deleteLater()` posted then never runs and leaks the whole thread data
  (`Mailbox` did this; LeakSanitizer found it).
- **TSan needs a TSan-instrumented Qt.** With a stock Qt every queued
  hand-off is reported as a race and the TSan runtime even aborts on
  thread creation — not a usable signal.
- **moc mis-reads raw string literals** containing quotes in files with
  `Q_OBJECT` (empty `.moc`, "undefined vtable"). Keep XML fixtures in
  `tests/data/`, not in `R"(…)"` literals in test sources.

## References

- Amazon S3 API reference: <https://docs.aws.amazon.com/AmazonS3/latest/API/>
- SigV4 signing: <https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv.html>
- Hetzner Object Storage docs: <https://docs.hetzner.com/storage/object-storage/>
- QtKeychain: <https://github.com/frankosterfeld/qtkeychain>
- Qt6 Widgets: <https://doc.qt.io/qt-6/qtwidgets-index.html>
