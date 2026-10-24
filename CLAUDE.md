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
**M1 done** — devlog 003 (library, accounts), devlog 004 (Hetzner
validation, nbg1).
**M2 done** — devlog 005 (browser UI, the mutation gate, create folder /
delete / rename, Hetzner run).
**M3 done** — devlog 006 (transfer engine: multipart, resume after a crash,
uploads / downloads, transfer panel, drag & drop in, incomplete uploads).
**M4 done** — devlog 007 (copy / move, also between buckets; clipboard and
internal drag & drop; headers / metadata / storage class edits; tags and
canned ACLs; presigned links; folder size).
**M5 done** — devlog 008 (versions view and tab, download / link / restore
a version, delete versions for good, undelete, the versioning switch in
Bucket Properties).
**M6 done** — devlog 009 (bucket policy, CORS, lifecycle, access, object
lock in Bucket Properties; retention and legal hold per object; the
governance bypass; creating and deleting buckets).
**M7 done** — devlog 010 (drag out: virtual files into Explorer, staged
copies on Linux / Flatpak; open in the default app and upload the changes;
bookmarks and recent locations; importing accounts from the AWS CLI and
rclone).
**M8 done — released as 1.0.0** — devlog 011 (`qbucket --self-test`; .deb
for Ubuntu 26.04 and the Flatpak bundle, built and smoke-tested in Docker;
the Windows release script and Inno Setup installer; README, CHANGELOG,
`docs/RELEASING.md`).
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
- Browser (M2): `controllers/AccountSessions` (one `s3::Session` per
  account), `models/AccountTreeModel` + `ui/AccountSidebar`,
  `models/ObjectTableModel` (paging via `loadMore`, own sort / filter,
  listing cache), `controllers/BrowseController`, `ui/ObjectBrowser`,
  `ui/PathBar`, `ui/ObjectRowDelegate`, `ui/DetailPane`, Edit menu and
  list keys.
- Capabilities in the app (M2): `controllers/CapabilityController`
  (probe, re-validate, merge, history, `decide()` / `reason()`,
  `ensureFresh()`), `accounts/CapabilityStore`, `ui/CapabilitiesDialog`,
  `controllers/ActivityLog` + `ui/LogPanel` (Log tab).
- The mutation gate (M2): `controllers/OperationController`, `Job`
  (plan → preview → execute → verify → report), `Journal` (JSONL),
  `PlanFile`, `ui/OperationPreviewDialog`; jobs `CreateFolderJob`,
  `DeleteJob`, `RenameJob`. `tools/check-mutation-gate.sh` (ctest
  `mutation_gate`).
- Transfers (M3), library: `Session` streams file ranges / into files on
  transfer lanes, multipart + part-number reads, `testing::FakeObjectStore`;
  `qbucket::s3_transfer` with `Uploader`, `Downloader`, `Copier`,
  `PartPlanner`, `AdaptiveConcurrency`, `JsonFileResumeStore`;
  `examples/s3cp`.
- Transfers (M3), app: `controllers/TransferService` (limiter per account,
  resume store), `UploadJob` (`LocalScanner`, conflict merge, policies),
  `DownloadJob` (`LocalPathMapper`), `TransferJobStore` (resume after quit /
  crash), `DiscardTransfersJob`, `AbortUploadsJob`; `models/TransferModel`,
  `ui/TransferPanel`, `models/MultipartUploadModel`,
  `ui/IncompleteUploadsDialog`; drag & drop in (browser, sidebar);
  Settings → Transfers. `bench/bench_transfer`.
- Object operations (M4), library: `ObjectHeaders`, CopyObject with
  `MetadataDirective::Replace`, object tags / ACLs on `Session`, write-only
  features `ObjectTaggingWrite` / `ObjectAclWrite`, `Copier` with replaced
  headers, `ProviderProfile::storageClasses()` / `cannedObjectAcls()`.
- Object operations (M4), app: `Job::buckets()` (the gate checks each),
  `ObjectCopyRunner`, `CopyJob`, `EditObjectsJob` (+ `ObjectSnapshot`),
  `ObjectPropertyJob`, `PresignService`, `FolderSizeTask`;
  `models/ObjectSelection` (clipboard / drag), `ui/EditMetadataDialog`,
  `ui/PresignDialog`, DetailPane Tags / Access tabs and "Calculate Size".
- Versions (M5), library: `ListObjectVersions` + `ListObjectVersionsPager`
  (pages put in S3's order), delete markers on `Error`,
  `Get/PutBucketVersioning`, write-only `BucketVersioningWrite`; the
  `FakeObjectStore` keeps versions.
- Versions (M5), app: `ObjectTableModel::Mode::Versions`, DetailPane
  Versions tab, `VersionWalk`, `DeleteVersionsJob` (versions / everything /
  undelete), `RestoreVersionsJob` (on `InPlaceCopy`), version downloads and
  links, `SetBucketVersioningJob`, `ui/BucketPropertiesDialog`,
  `ui/UndeleteDialog`; `CapabilityController::versioningOf()`.
- Bucket management (M6), library: `BucketConfig.h` (one transport for
  every configuration document, typed lossless-or-saying-so views of CORS,
  lifecycle, object lock, public access block; policy checks; bucket name
  rules), write guards (answers like CreateBucket / DeleteBucket are
  Protocol errors), eight write-only `*Write` features, object retention /
  legal hold, the governance bypass on deletes, `CephProfile`,
  `ProviderProfile::bucketEmptinessSettleSecs()`.
- Bucket management (M6), app: `SetBucketConfigJob`, `CreateBucketJob`,
  `DeleteBucketJob`, `ObjectLockJob`, `VersionPurger` (shared with
  `DeleteVersionsJob`); `Preview::bypassOffer`; Bucket Properties pages
  (`ui/BucketConfigPage` + `PolicyPage`, `CorsPage`, `LifecyclePage`,
  `AclSection`, `PublicAccessBlockSection`, `ObjectLockPage`,
  `OtherConfigPage`, `ui/CodeEditor`), `ui/CreateBucketDialog`, the
  Bucket menu, the DetailPane Lock tab.
- M7, library: `GetObjectParams` (ranged reads into memory, If-Match),
  `PutObjectParams::headers` / `cannedAcl` / `serverSideEncryption` /
  `ifMatch` (also on `UploadRequest`, Complete, `TransferRecord`), the
  write-only feature `ConditionalOverwrite`, `ProviderRegistry::
  matchEndpoint`, `AwsConfigFiles`.
- M7, app: `src/platform/` (`qbucket_platform`: `ExternalDrag`,
  `StagedDrag` + `StagingMimeData`, `VirtualFileReader` over the plain-C++
  `VirtualFileSource`; `platform/win/`: `VirtualFileDataObject`,
  `RangeReaderStream`, `DropSource`, `VirtualFileDrag`),
  `controllers/DragStaging`, `OpenFileService`, `ReplaceObjectJob`,
  `AccountImporter`, `accounts/LocationStore`, `RcloneConfigReader`;
  `ui/ObjectTableView`, `ImportAccountsDialog`, `OpenFilesDialog`,
  `ManageBookmarksDialog`, the Bookmarks menu and sidebar section, the bar
  for changed files; `DownloadJob::Options::transient` / `listDone`.
- `bench/bench_listing` — the §13.4 budgets against a FakeS3Server (built
  with the tests, run by hand, release builds only; `--version-keys` for
  the versions view).
- M8: `controllers/SelfTest` (`--self-test`), `platform/Console`,
  `platform/KeychainService` (does a keychain service exist? —
  `QtKeychainBackend` turns a failure without one into NoBackend);
  `packaging/debian/` (control, rules, man page, `build-deb.sh`,
  `build-in-docker.sh`), `packaging/flatpak/build-in-docker.sh` + smoke
  container, `packaging/flathub/` (prepared, not submitted),
  `packaging/windows/` (`build-release.ps1`, `installer.iss`);
  `tools/check-version.sh`, `tools/tidy-in-docker.sh`;
  `test_release_screenshots` (→ `docs/img/`).
- Workflows: `ci.yml` per push (Linux Release, Windows app, library as
  Windows DLL), `ci-extended.yml`, `s3-integration.yml`. **GitHub Actions
  don't run at the moment (no build minutes)**: every check runs locally
  before a commit — the list in `docs/RELEASING.md`; Windows is checked by
  the user. The user pushes.

Provider knowledge: `docs/providers/hetzner.md`.

Next: nothing planned — the post-parity backlog (PLAN.md §6) on request.
Releases: `docs/RELEASING.md`.

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

CI runs only the essentials on every push (`ci.yml`). Run the rest locally
when a change touches what they guard — threading/lifetimes, the library
protocol, packaging (Linux commands; all builds outside the source tree):

```sh
# library alone, with ASan + UBSan + LeakSanitizer (lifetimes, threads)
cmake -S libs/qbucket-s3 -B build-s3-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DQBUCKET_S3_WARNINGS_AS_ERRORS=ON -DQBUCKET_S3_SANITIZE=address,undefined
cmake --build build-s3-asan && ctest --test-dir build-s3-asan
# Release build with -Werror (GCC -O3 warns differently than Debug)
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DQBUCKET_WARNINGS_AS_ERRORS=ON
cmake --build build-release && ctest --test-dir build-release
# clang-tidy 18 like CI (pip/wheel `clang-tidy==18.1.8`, binary via CLANG_TIDY),
# on build/compile_commands.json; CI runs it as 4 shards (`… build 0 4`).
# Check the exit code — a missing binary must not look like "no findings".
tools/run-clang-tidy.sh build
# … or without a local clang-tidy: the same from a Docker image (Ubuntu 26.04
# with its Qt — the host's build directory is used as it is; ~10 min)
tools/tidy-in-docker.sh run-clang-tidy.sh build
tools/tidy-in-docker.sh check-msvc-shadow.sh build FILE…
# real S3 servers in Docker (Ceph RGW, VersityGW, Garage): the library's
# it_s3_backend and the app's jobs (it_operations)
libs/qbucket-s3/tests/docker/it-up.sh
cmake -S . -B build -DQBUCKET_S3_IT_LOCAL=ON && cmake --build build
ctest --test-dir build -L docker
# Hetzner (qbucket-it-* buckets with a guard object only):
#   set -a; . ~/.config/qbucket-it/hetzner.env; set +a
#   build/libs/qbucket-s3/tests/it_s3_backend    (+ QBUCKET_IT_SECOND_BUCKET=qbucket-it-lock
#                                                   QBUCKET_IT_VERSIONING_BUCKET=qbucket-it-versioning
#                                                   QBUCKET_IT_CONFIG_BUCKET=qbucket-it-versioning
#                                                   QBUCKET_IT_LOCKCFG_BUCKET=qbucket-it-lockcfg
#                                                   QBUCKET_IT_TEMP_BUCKETS=1)
#   build/tests/it_operations (same variables); build/tests/it_browser   (it_browser: read-only account)
# listing budgets (release build; offscreen — the budgets' platform)
QT_QPA_PLATFORM=offscreen build-release/bench/bench_listing --keys 100000 --flat-keys 1000000 --version-keys 250000
# transfer budgets: GUI stalls while planning 1M files / uploading / browsing,
# planning a 100k-object copy, counting a 1M-object folder
build-release/bench/bench_transfer --files 1000000 --upload-mib 2048 --size-keys 1000000
# a transfer by hand, killable and resumable (credentials from AWS_* env)
build/libs/qbucket-s3/tests/s3cp/s3cp --endpoint … --resume-dir DIR --id X upload FILE BUCKET KEY
build/libs/qbucket-s3/tests/s3cp/s3cp --endpoint … --resume-dir DIR resume X
```

Packages, built and smoke-tested in Docker (nothing installed on the host;
`dist/`), and on Windows by hand — `docs/RELEASING.md`:

```sh
packaging/debian/build-in-docker.sh     # .deb for Ubuntu 26.04 (~15 min)
packaging/flatpak/build-in-docker.sh    # Flatpak bundle (~15 min)
# Windows: packaging\windows\build-release.ps1 (ZIP + installer)
# the README / AppStream screenshots (real platform)
QT_QPA_PLATFORM=xcb QBUCKET_SCREENSHOT_DIR=docs/img build/tests/test_release_screenshots
```

The extended CI (`ci-extended.yml`, `s3-integration.yml`) can be started by
hand from the Actions tab (`workflow_dispatch`) — once Actions run again.

## Conventions

Inherited from xyz-profiler — keep both projects consistent.

- `namespace qbucket`. **No `Q` class prefix** — that reads as Qt API.
- Layering: library `qbucket-s3` ← app `domain` ← `accounts` ←
  `controllers` ← `models` ← `ui`. Nothing below `ui` includes widget
  headers; models reach S3 only through `AccountSessions`.
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
- **`QTreeView` calls `fetchMore()` from every layout pass**
  (`QTreeViewPrivate::layout`) — with asynchronous pages it pages through
  the whole bucket. `ObjectTableModel` doesn't implement
  `canFetchMore`/`fetchMore`; the browser calls `loadMore()` near the end
  of the list. For large flat lists use **`QTableView`**: the tree relays
  out every row item after each inserted page (47 ms vs. 12 ms at 1M rows).
- **No `QSortFilterProxyModel` over big listings**: it sorts synchronously
  on the GUI thread. `ObjectTableModel` sorts itself (thread pool above
  5 000 rows, applied with `layoutChanged`).
- **`QCollator::setNumericMode` needs ICU** — the Qt of the CI installer
  has none and silently sorts "file10" before "file9". Natural order is
  `ObjectTableModel`'s own digit-run comparison.
- **`windows.h` defines `min` / `max`** — targets including it get
  `NOMINMAX`.
- **MSVC C4458: a local `d` in a `Foo::Impl` method hides `Foo::d`** (the
  PIMPL pointer) — GCC and clang say nothing, `/WX` breaks the Windows
  build. Before pushing much new code, check with clang:
  `tools/check-msvc-shadow.sh build FILE…` (looks for "shadows a field";
  the lambda "uncaptured local" hits are fine on MSVC). It fails when
  clang-tidy didn't run — without a real check enabled, clang-tidy only
  prints its usage and a hand-rolled grep finds "nothing".
- **`QAbstractItemModelTester` re-checks the whole model on every change**
  (30 s with thousands of rows) and calls `fetchMore` inside its own reset
  handling — use it on small models; `fetchMore` must not insert rows
  synchronously.
- **A widget hidden by a `QStackedWidget` loses the focus** (the list
  swapped for "Loading…" → Backspace dead). Park the focus on the message
  page and hand it back.
- **Two shortcuts with the same key are none** — a window shortcut plus a
  widget shortcut for F5 is "ambiguous" and fires neither.
- **Theme names are `Dark` / `Light` / `System`**; anything else paints
  dark.
- **Plural forms need `translations/qbucket_en.ts`** (plurals only, loaded
  before the locale's file), or English shows "1 object(s)". New `%n`
  strings: run `update_translations`, fill both `.ts` files.
- **`QSignalSpy::wait()` waits for the *next* emission.** A job that
  finished while a `QTRY_*` loop spun has already emitted: check
  `spy.size() == 1 || spy.wait()`. And jobs are `deleteLater`'d after
  `finished` — never read one in a failure message after waiting.
- **A request still under way must not re-create a file its owner
  removed.** The download sink opens `.qbpart` with `ExistingOnly`
  (`GetObjectToFileParams::createFile = false`); otherwise a range started
  just before a cancel brought the deleted partial file back.
- **Run test binaries by hand with `QT_QPA_PLATFORM=offscreen`** (ctest
  sets it). On the real desktop (KDE / Wayland) an unfocused test window's
  clipboard never reaches the compositor and Klipper hands back its own
  content — clipboard tests fail there for no fault of the code.
- **Heavy work one after the other, at `-j12`.** Two builds at `-j20` side
  by side plus a 1M-row benchmark took the machine down (31 GB, no swap).
  A reboot empties `/tmp` — the scratchpad's clang-tidy too; the scripts
  then fail loudly.
- **Debugging a hanging QtTest binary:** after an abort, QtTest's crash
  handler forks to print a stack and can deadlock on the malloc lock —
  `QTEST_DISABLE_STACK_DUMP=1` shows the real failure.
- **`QLatin1String` with a UTF-8 literal** ("…", "“") compares wrong: the
  bytes are read as Latin-1. Use `QStringLiteral` / `u"…"` (a test failed
  finding "&Open Files…" by text).
- **`QMenu::clear()` keeps submenus alive** (only the actions go): a menu
  rebuilt on `aboutToShow` keeps one submenu member instead of adding a
  new one each time.
- **lupdate sees only sources the build system compiles.** Texts of
  Windows-only files (`platform/win/`) live in the platform-neutral base
  (`ExternalDrag`), or the Linux `update_translations` never finds them.
- **Windows code can be checked on Linux:** llvm-mingw (Docker image
  `mstorsjo/llvm-mingw:20261006`, the Linux Qt headers mounted) with
  `-fsyntax-only -Wshadow-all -D_WIN32_WINNT=0x0A00` compiles the COM layer,
  and its `windres` the rc file; a real Windows build stays the judge (MSVC
  `/WX`, `test_virtual_file_drag` — `build-release.ps1`).
- **A `#primary` button needs its own `:disabled` rule** — the accent
  background otherwise makes a disabled one look enabled.
- **The tab-bar base ignores the palette.** With `setDocumentMode(true)`
  the style paints the tab bar's base line in a fixed `#393e43` in both
  themes, and `QTabWidget::pane { border-top }` has no effect. The QSS
  sets `QTabBar { qproperty-drawBase: 0; border-bottom: … @border@ }`;
  test `tabBarsUseThemedSeparator` guards it. (Same in xyz-profiler.)
- **QtKeychain on Linux never says "no backend".** A missing keyring daemon
  comes back as `OtherError` ("Unknown error", or the D-Bus error), so
  SecretStore's session-only mode never started and an account couldn't be
  saved. `QtKeychainBackend` asks `platform::checkKeychainService` on every
  failure (devlog 011). Found only by installing the .deb in a clean
  container — test packages where nothing else is installed.
- **D-Bus in tests: a bus of its own needs a config of its own.**
  `dbus-daemon --session` reads the system's service directories (this
  machine's keyring becomes activatable), and a policy without receive rules
  drops the replies — a check then fails for the wrong reason.
- **flatpak-builder's `--state-dir` must be on the build directory's file
  system** (both in one Docker volume).
- **ISCC and rc.exe read BOM-less files as ANSI** — `installer.iss`,
  `app.rc.in` and `build-release.ps1` (Windows PowerShell 5.1) stay ASCII.

## Production data — safety rules

QBucket almost always runs against **production data**. Robustness beats
convenience and speed: **check once too often rather than once too
little.** Full rules in `PLAN.md` §12; the ones every change must respect:

- **One mutation gate:** every write/delete/copy/config change is a `Job`
  of the `OperationController` (read-only accounts, capabilities,
  environment marker, confirmation, journal, verification). The UI never
  calls the library's mutating API directly — `tools/check-mutation-gate.sh`
  (ctest `mutation_gate`) fails on such calls outside `src/controllers/`.
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
- **A read probe never unlocks a write.** PUT/DELETE on a bucket
  subresource only for features with confirmed *write* support (profile or
  deep probe). Hetzner's proxy executed `DELETE /?encryption` as
  DeleteBucket (`docs/providers/hetzner.md`). Write probes only on a scratch
  bucket that holds a guard object.

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
  anywhere in the app — with one exception: `ProviderRegistry::
  matchEndpoint` (an exact match against the profiles' preset endpoints)
  suggests a profile when an account is set up (the import), confirmed by
  the user. `tools/check-mutation-gate.sh` keeps it in `AccountImporter`.
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
- **CopyObject with `x-amz-metadata-directive: COPY` does not keep the
  storage class** (the copy lands in the default class) and never copies
  ACLs. Pass the class along; compare size + (single-part) ETag before
  deleting a source.
- Hetzner copies between buckets of one project, although its docs say
  otherwise (devlog 005; `it_s3_backend crossBucketCopy`).
- **Hetzner and Garage ignore `If-None-Match: *`** — the PUT overwrites.
  Their profiles know `ConditionalWrites` as Unsupported; uploads check an
  existing key with HEAD right before writing instead (devlog 006).
- Multipart requests keep their query string through Hetzner's proxy
  (UploadPart / Abort on an existing key leave it intact); HEAD / GET
  `?partNumber=` work on Hetzner, Ceph, VersityGW and Garage.
- A multipart object's MD5-of-MD5s can be checked after a download when
  the ranges follow its part layout (HEAD `?partNumber=1`); otherwise only
  the size — reported as such.
- **CompleteMultipartUpload can take minutes without a byte on the wire**
  (VersityGW: ~6 min for 50 GiB). Complete / CopyObject / UploadPartCopy
  use `SessionOptions::serverWorkTimeoutMs` (15 min), not the 120 s
  transfer timeout — a client that hangs up makes the server cancel.
- **VersityGW on ext4 fails Completes above ~640 parts** (500, ENOSPC from
  an xattr in its log). Big multipart runs locally: Garage.
- **Tag / ACL writes have write-only features** (`object.tagging-write`,
  `object.acl-write`): a read probe never sets them; a profile does, after
  canary + victim ITs on guarded buckets (Hetzner: confirmed 2026-10-21).
  No `DeleteObjectTagging` — an empty tag set removes the tags.
- **An edit is a copy onto itself with REPLACE**, which keeps only what it
  sends: merge into a fresh snapshot (HEAD + tags + ACL), send the canned
  ACL and SSE-S3 along, read everything back. **RGW (Ceph, Hetzner) drops
  the tags on such a copy** whatever the tagging directive, and Hetzner
  leaves tags sent with `x-amz-tagging` unreadable (`GET ?tagging` → empty
  200): write them back with `PutObjectTagging`. VersityGW keeps standard
  headers a REPLACE leaves out; Garage ignores the storage class on copies.
- **If-Match on writes is unusable on RGW** (Hetzner, Ceph 19.2): PutObject
  compares it with the *unquoted* ETag (the RFC form is refused even when it
  matches), CompleteMultipartUpload ignores it; Garage ignores it entirely.
  `ConditionalOverwrite` is Supported for AWS only (it_s3_backend
  `conditionalOverwrite`, VersityGW honours it); elsewhere a replace checks
  the ETag with HEAD right before the write.
- Storage classes per provider: Hetzner and Garage only STANDARD (Hetzner
  answers 400 InvalidArgument to STANDARD_IA).
- **Versions:** a delete marker is only certain from `ListObjectVersions`
  — RGW (Ceph, Hetzner) and VersityGW answer HEAD on a deleted key with a
  bare 404. RGW sends no version ids on CopyObject. VersityGW lists a
  page's markers before its versions (the parser re-sorts). `PUT
  ?versioning` needs `bucket.versioning-write` (Hetzner: confirmed by
  canary + toggle on `qbucket-it-versioning`, 2026-10-21); never sent with
  MfaDelete. Destructive version jobs need the bucket probed first.
- **Bucket configuration writes have write-only features** (one per kind,
  PUT and DELETE): a read probe never sets them; a profile does after a
  canary + write / read / remove on a guarded scratch bucket (Hetzner: all
  confirmed 2026-10-21; Ceph: all; Garage: CORS and lifecycle; VersityGW
  without lifecycle, public access block, ACL). A removal is followed by a
  look at the bucket. Editors never drop what their form can't show
  (`lossless`).
- **Hetzner's DeleteBucket deletes a bucket that looked empty within the
  last ~10 s — with the objects just written** (the proxy decides by lagging
  statistics). `bucketEmptinessSettleSecs()` (Hetzner 20): list it empty,
  wait, list again, then delete; never trust BucketNotEmpty alone there.
  A guard object protects only once it is older than that.
- Hetzner: a default retention reaches new versions only after up to
  ~1.5 min; one gateway may not know an hour-old bucket (empty-message
  NoSuchBucket, retried). COMPLIANCE is never set on a real server in tests.
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
