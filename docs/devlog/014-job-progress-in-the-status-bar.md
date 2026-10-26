# Devlog 014 — What a running job is doing, in the status bar

**Date:** 2026-10-26

## What was built

After you confirmed that a bucket and everything in it should be deleted,
the window looked idle. The job worked correctly in the background. The
only sign of it was a short label at the right end of the status bar
("Delete bucket “x” with everything in it (5000 of 5000)"), and that label
stopped changing exactly where the run takes longest:

- once the contents are gone, `DeleteBucketJob` lists the bucket again;
- it then waits for the server's emptiness check
  (`bucketEmptinessSettleSecs`, 20 s on Hetzner);
- it lists once more, sends DeleteBucket, and checks with HEAD until the
  bucket is gone.

The success message then showed for only 10 s.

- **`Job::step()`** — a short text for what the job is doing right now,
  where the phase says too little. `setStep_()` emits `progressChanged`.
  Every phase change clears the step, so it never outlives its stage.
- **`DeleteBucketJob`** sets a step at each stage: listing the bucket /
  the unfinished uploads, deleting the contents, aborting uploads, checking
  that the bucket is empty (and again, when the listing lags), "waiting
  %n s for the server to confirm the bucket is empty", checking that it is
  still empty, deleting the bucket, checking that it is gone.
- **Status bar:** a `QProgressBar` (`jobProgress`) beside the label. It
  fills while the rest can be counted and is busy otherwise (listing,
  waiting, checking). The label reads "title (step, 120 of 5 000)", with
  numbers formatted by the locale; its tooltip holds the full text.
  Bar and Cancel are slim (QSS `QStatusBar QProgressBar` 6 px,
  `QStatusBar QToolButton` without vertical padding), so a running job
  doesn't make the status bar taller. Qt adds the border to `max-height`:
  the bar is 8 px in all.
- **A deleted bucket ends with a message box** ("The bucket “x” was
  deleted.", with the summary). It opens with `open()` and doesn't block.
  Failures already showed one; a cancel stays a status line.

## Decisions and why

- **The status bar, not a dialog or the transfer panel** (the user's
  choice). The bar now moves through every stage, the label names the
  stage, and only the end of a bucket deletion gets a dialog.
- **The count is shown only while something is left to count.** "5 000 of
  5 000" next to "waiting for the server" reads as stuck. The bar is
  busy then, too.
- **Per mille on the bar.** `QProgressBar` takes `int`; plans are counted
  in `qint64`.
- **Text first, then show.** `updateJobStatus_` used to make the label
  visible before setting its text, so a new job could flash the last job's
  text for a moment.

## Refreshing after a bucket deletion

The bucket view was said not to update after a deletion. Two suspects:

- **ListBuckets lagging behind DeleteBucket**, as Hetzner's emptiness
  check does. Measured against Hetzner (nbg1) with `curl --aws-sigv4`: an
  empty bucket was missing from the list one second after the 204. The
  whole window flow was run against Hetzner too — a bucket with two
  objects, sidebar request, preview, job: one second after the end the
  sidebar listed the other buckets only, and the browser had left it. No
  lag, so no grace period in `AccountTreeModel` (written, then taken out
  again).
- **A real gap in `MainWindow`'s `jobFinished`:** for a bucket deletion it
  refreshed the sidebar and returned. The browser left the bucket on
  success only; when the job deleted the contents but the bucket stayed
  (cancelled, a version under retention, "something appeared meanwhile"),
  the browser went on showing the deleted objects. Now it lists the bucket
  afresh. `aBucketThatStaysIsListedAfresh` (object lock, one version under
  GOVERNANCE retention) fails without the fix.

## Pitfalls

- **lupdate on Windows** also picks up QtKeychain's Windows sources and
  moves hundreds of location lines. The `.ts` files are kept by the Linux
  run; on Windows, add the new entries by hand.

## Tests

- `test_bucket_jobs`: `emptyThenDelete` checks that the steps come in
  order; `theSettleWaitIsAStepOfItsOwn` checks the waiting step.
- `test_main_window_operations`: `deletingABucketShowsProgressAndTheEnd`
  runs a bucket with contents through the sidebar request, the question,
  the preview (typed name, production) and the job. It checks that the bar
  showed with a label, that it hides again, and that the message box
  appears.
