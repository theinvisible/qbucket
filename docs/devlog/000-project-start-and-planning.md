# Devlog 000 — Project start & planning

**Date:** 2026-10-19

## What this is about

A native open-source file browser for S3 and S3-compatible object storage. The feature
yardstick is NetSDK's **S3 Browser** (policies, CORS, lifecycle, versioning, ACLs, multipart,
presigned URLs, …), which is Windows-only and paid for commercial use. QBucket is meant to do
the same on **Windows and Linux** under the **MIT** license.

Name: **QBucket**, a nod to Qt. Availability check: no product of that name; winget,
Flathub, PyPI, npm and crates.io are free. The GitHub account `qbucket` is taken by an
inactive user, so the repo lives under `theinvisible/qbucket`.

## Stack decision

Four options were compared, estimated to S3 Browser parity:

| Option | Estimate | Verdict |
|---|---|---|
| Rust + egui | 2–3 months | `aws-sdk-s3` is strong, but tables/forms are laborious and there's no drag-out (winit) |
| **C++ + Qt6 Widgets** | **1.5–2 months** | **chosen:** tables, trees, forms, D&D are native; toolchain already set up from xyz-profiler |
| Go + Gio | 2.5–3.5 months | minio-go is ideal, but few widgets, no OS drag & drop, API still pre-1.0 |
| Go + miqt | 1.5–2 months | Qt widgets + minio-go is tempting, but the binding is young with a bus factor of 1 |

The S3 layer uses **no AWS SDK** but an own client on `QNetworkAccessManager` with a SigV4
signer and `QXmlStreamReader`. aws-sdk-cpp is painful to build and brings its own threading
model; minio-cpp depends on curlpp and OpenSSL. An own client keeps provider quirks fully under
our control.

## Ground rules

- **The S3 layer is a reusable library**, `libs/qbucket-s3` (Qt Core/Network only, MIT,
  own semver), so other projects can use it. Credentials, auditing and resume storage plug in
  through interfaces; protocol-level safety lives in the library, policy (read-only,
  confirmations) in the app. A standalone CI job enforces the boundary.
- **Dynamic capability safeguard:** on connect / bucket open the server's features are probed
  read-only (GET subresources, response root element checked); unsupported features make the
  action impossible — enforced in the library, the `OperationController` and the UI, with the
  reason shown. Unknown state never unlocks destructive operations. Results expire and are
  re-checked automatically (server features daily, bucket state after 10 minutes, immediately
  when the server fingerprint changes), so a provider upgrade that adds features is picked up
  on its own and reported to the user.
- **Design** ported 1:1 from xyz-profiler: `Theme` palette (accent `#2563EB`),
  `DarkFusionStyle` QSS, `IconFactory` line icons, Dark / Light / System.
- **Hetzner Object Storage first.** Other providers come in exclusively through
  `ProviderProfile` classes; the core never checks which provider it talks to.
- **Linux ships as a Flatpak first**, app id `io.github.theinvisible.QBucket`. The Flatpak
  skeleton lands in M0 so sandbox and portal issues surface early.
- **Passwords and secret keys only via QtKeychain** (`SecretStore`), no plaintext fallback.
- **Production-data rule:** robustness before convenience and speed — "check once too often
  rather than once too little". Consequences: a single gate for all mutations
  (`OperationController`), plan → preview → execute → verify → report, a journal,
  `Content-MD5` on every upload, read-only accounts and an environment marker.
- **Performance with millions of objects** comes second, with fixed budgets (e.g.
  < 40 MB per 100k rows, GUI stalls < 50 ms).
- **The UI never blocks:** `S3Client` with its own QNAM on worker threads (browse thread per
  account, transfer pool), an I/O pool for hashing, disk I/O and sorting. The GUI thread only
  paints.

## Plan

Milestones M0–M8, details in [PLAN.md](../../PLAN.md):

| | Content |
|---|---|
| M0 | Skeleton, design port, empty main window, CI Windows/Linux, Flatpak skeleton |
| M1 | S3 core: SigV4, worker-thread client, provider profiles, Hetzner capability matrix, keychain |
| M2 | Browser UI with `fetchMore`, OperationController, read-only / environments |
| M3 | Transfer engine: multipart, resume, integrity checks, journal |
| M4 | Object operations: copy/move, metadata, tags, ACL, presigned URLs |
| M5 | Versioning |
| M6 | Bucket management (policy, CORS, lifecycle, object lock, …) |
| M7 | Drag & drop out, bookmarks, account import |
| M8 | Flathub, Windows installer, release |

Estimate: about **6 weeks** to a daily-driver build (M0–M3) and about **11 weeks** to parity,
roughly 2 weeks of which go into the robustness work.

## Test backends

Early development and testing happen on **Linux with Docker**: a real single-node Ceph cluster
with RGW in our own image on top of the official `quay.io/ceph/ceph` (closest to Hetzner, which
runs RGW), VersityGW (broad S3 features) and Garage (deliberately limited, to exercise
the "not supported" paths). Hetzner follows as the reference. MinIO is out (community repo
archived in 2026, no official images), LocalStack too (auth token required since 2026.03).

Hetzner's "supported actions" page already shows quirks the plan must handle, most notably
**CopyObject only within the same bucket**, so cross-bucket copies fall back to download +
upload. Other limits: SSE-C only, canned ACLs `private` / `public-read` only, a narrow
lifecycle subset (details in `PLAN.md` §3.2).
