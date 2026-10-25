# Devlog 012 — M8: the Windows release, built and checked on Windows

**Date:** 2026-10-25

## What was built

Devlog 011 left Windows "prepared, not run": `build-release.ps1` had only
been parsed by PowerShell in Docker and `installer.iss` compiled under Wine.
This step ran it on a Windows 11 machine (Visual Studio 2022, Qt 6.12.0
msvc2022_64, Inno Setup 6.7.3). That was also the first time the whole test
suite ran on Windows: since M0 the Windows code had been checked with
llvm-mingw's syntax check only. The run found seven failing tests — a
crash, a race, a protocol hole — and three packaging gaps.

| Stage | What |
|---|---|
| W1 | `build-release.ps1`: windeployqt into an empty folder, the MSVC runtime app-local, a `dumpbin` dependency check, the staged exe run with nothing but Windows on `PATH` |
| W2 | windeployqt without the DXC shader compiler (`--no-system-dxc-compiler`, also for dev builds) |
| W3 | `test-installer.ps1`: per-user install of an older version, the update, the uninstall |
| fixes | `OneShotBody` (QNAM's own resend), monotonic upload progress, `mkpath` race, `~MainWindow` order, three test fixes, Qt 6.12 deprecations |

## Decisions and why

- **The MSVC runtime app-local, not `vc_redist`.** windeployqt puts only
  `vc_redist.x64.exe` next to a Release build, which nothing ran; the ZIP and
  the installed folder would not start on a clean Windows. Running vc_redist
  from the installer needs admin rights, which the per-user install avoids,
  and leaves the ZIP without a runtime. The DLLs of
  `VCToolsRedistDir\x64\Microsoft.VC143.CRT` are now staged next to
  `qbucket.exe`.
- **A dependency check instead of a clean machine.** There is no clean
  Windows here (no VM, no Windows Sandbox). The gap above is invisible on a
  development machine: Visual Studio put the runtime into System32, so the
  copy starts there. `Test-StageDependencies` reads every staged exe / dll
  (plugins too) with `dumpbin /dependents`. Each import must be staged,
  an `api-ms-win-*` / `ext-ms-*` set, or a System32 DLL that isn't the MSVC
  runtime. Counter-check: without the runtime DLLs it lists 41 missing
  imports.
- **Staged afresh.** windeployqt runs into an empty `dist\stage\qbucket`
  instead of copying `*.dll` and a fixed list of plugin folders out of a
  reused build directory, where a former Qt or former flags leave files
  behind. Non-Qt DLLs that `qbucket.exe` imports would be copied from the
  build directory or Qt's `bin` (none today: QtKeychain is static).
- **No DXC.** `dxcompiler.dll` and `dxil.dll` (13.2 MiB) are for the D3D12
  RHI; a Widgets application paints in software. Dropped with
  `--no-system-dxc-compiler` (Qt 6.6+; the script asks windeployqt whether
  it knows the option).
- **The installer tested on this machine, per user.** `test-installer.ps1`
  builds a 0.9.0 setup from the same staged folder, installs it silently
  (`/CURRENTUSER`), plants a stale plugin, installs 1.0.0 over it (one
  uninstall entry, the right version, the stale plugin gone, every staged
  file there, `--version` and `--self-test` of the installed exe),
  uninstalls (folder, entry and Start menu entry gone, the settings folder
  unchanged). It refuses to run while QBucket is installed; a real
  installation is never touched. "For all users" (UAC) stays a check by
  hand.
- **No code signing yet** — the setup and the exe are unsigned, SmartScreen
  warns. `build-release.ps1` has no signing step until there is a
  certificate.

## What the first full Windows test run found

The first release run stopped at `/WX`, then failed 7 of 94 tests.

- **Qt sends a non-idempotent request again by itself.** `test_s3client_faults
  lostReplyOfNonIdempotentRequestIsNeverRetried` and `test_s3client_write
  copyObjectFailuresAreNeverSuccesses`: the server saw two requests where
  the library made one attempt. QHttpNetworkConnectionChannel resends a
  request when the connection closes before the response — any method, an
  RST as well as a FIN. Linux with Qt 6.10 never showed it; on Windows with
  Qt 6.12 it was deterministic. A CompleteMultipartUpload, CopyObject or
  CreateBucket the server had executed would have been sent again, and the
  second answer (NoSuchUpload, 409 BucketAlreadyOwnedByYou) taken as the
  truth. Nothing public tells: `requestSent` never fired, and
  `socketStartedConnecting` fired once for two sends. But before a resend
  QNAM rewinds the body, and when that fails it gives up with
  `ContentReSendError`. Non-idempotent requests now send their body —
  also an empty one — through `OneShotBody`, which refuses a rewind once
  anything was read, and any rewind of an empty body (QNAM rewinds an
  empty body only for the resend; the first send never does — every
  CopyObject test proves it). The error is `outcomeUnknown` as before, and
  the server sees one request. Test
  `theNetworkLayerNeverSendsANonIdempotentRequestAgain` (drop, then 409;
  with and without a body).
- **Upload progress went back to 0.** `test_session_transfer
  uploadProgressReachesTheTotal`: Qt 6.12 on Windows reports 0 again once
  the upload is over. The pipeline forwards only progress that grows.
- **Parallel downloads failed with "cannot create the folder".**
  `test_drag_staging` (now and then, 38 s): two downloads creating
  `photos/` and `photos/2026/` at once. Qt's `mkpath` on Windows recurses
  upwards when a parent is missing; if another thread creates that parent
  in between, its `mkdir` fails and the recursion climbs to the drive and
  gives up. `Downloader` and `DownloadJob` try up to three times and then
  look whether the folder is there. 10 / 10 runs green after (before: about
  every third run failed).
- **A crash closing the window with a job running.** `test_main_window_open`
  (5 of 10 runs). A build with debug info showed the stack: `~MainWindow` →
  QObject deletes the children in the order they were made →
  `AccountSessions` (made first) went first → `~OperationController`
  cancelled the running `ReplaceObjectJob` → `Uploader::cancel` →
  `Session::cancel` on a deleted session. The library says the session must
  outlive the uploader; the app broke that. `~MainWindow` now moves
  `AccountSessions` to the end of the children. Test
  `destroyedWhileAnUploadRuns` (PutObject held back 3 s): 5 / 5 crashes
  without the fix, 0 / 5 with it.
- **Three test faults.** `test_open_file_service` still held the file open
  when it expected it deleted (Windows deletes no open file).
  `test_virtual_file_drag createdInTheMtaUsedThroughAProxy` called
  `VirtualFileDrag::createDataObject` without holding the MTA the way
  `VirtualFileDrag` does; the MTA ended with the creating thread and the
  proxy answered `RPC_E_DISCONNECTED`. And Qt 6.12 deprecates the
  `QPoint` constructors of `QDragEnterEvent` / `QDragMoveEvent`
  (`tests/support/DragPosition.h` picks `QPointF` from 6.12 on).

## Measurements

- ctest 94 / 94 (Release, `/WX`, Qt 6.12.0); the library tests 36 / 36.
  The resend tests 15 / 15 runs green, `test_main_window_open` 15 / 15,
  `test_drag_staging` 10 / 10.
- Hetzner (nbg1) with the new pipeline, from Windows: `it_s3_backend`
  42 passed, 2 skipped (repeated slashes kept; no restricted user),
  temporary buckets on; `it_operations` 17 / 17; the guard objects of
  `qbucket-it-test`, `-lock`, `-versioning` and `-lockcfg` there afterwards.
- The release run green in Windows PowerShell 5.1 and in PowerShell 7.
- `dist\`: `qbucket-1.0.0-windows-x64.zip` 14.7 MiB (15.2 MiB from
  PowerShell 7, whose `Compress-Archive` packs less tightly),
  `qbucket-1.0.0-windows-x64-setup.exe` 12.0 MiB; the staged folder
  35.7 MiB (27 binaries) — 13.2 MiB less without DXC.
- The installer test, all steps green; `--self-test` of the staged and the
  installed exe: keychain round trip through the Credential Manager.

## Pitfalls

- **PowerShell doesn't wait for a GUI executable** started with `&`, and
  only a direct pipe captures its output: `--version` came back empty and
  `$LASTEXITCODE` was stale. The scripts start `qbucket.exe` with
  `Start-Process -Wait -RedirectStandardOutput`; by hand, `qbucket
  --self-test | Out-String` (README).
- **No public signal shows QNAM's resend** — the rewind of the body is the
  only hook. A body that refuses every rewind would break the first send
  if QNAM ever rewound before it; the CopyObject tests (empty body) would
  show it.
- **A Release build has no symbols** for QtTest's crash handler. A separate
  `RelWithDebInfo` build of the one test (`build-windows-rwdi`) named the
  frames at once.

## Still to check — by hand

- The setup "for all users" (UAC), start from the Start menu, an account,
  browse / upload / download, dragging into Explorer (one file, several, a
  folder, a large one; Escape and Explorer's Cancel), uninstall —
  `docs/RELEASING.md` §4.
- The changed shared code on Linux: debug, release `-Werror`, ASan,
  clang-tidy (`docs/RELEASING.md` §2) — this machine has no Docker.

## Open points

- Code signing (SmartScreen).
- A clean Windows (Sandbox or VM) would replace the dependency check's
  inference with a real start.
- `qbucket --self-test` in a console needs the pipe; a console twin
  (`qbucket.com`) would make it plain.
- Does Qt 6.10 / Linux resend too, on an RST? `OneShotBody` covers both
  either way.
