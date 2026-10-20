# Hetzner Object Storage

QBucket's first-class provider (PLAN.md §3.2). Encoded in
`libs/qbucket-s3`: `HetznerProfile` (+ `test_provider_hetzner`), fixtures in
`libs/qbucket-s3/tests/data/hetzner/`, integration expectations in
`libs/qbucket-s3/tests/data/it-expectations/hetzner.json`.

**Verified:** 2026-10-20 against a real project, location **nbg1**, with
`it_s3_backend` (12/12 on each bucket; with the M3 multipart tests 17
passed, 3 skipped for missing optional setup) and `s3ls probe`; fsn1 and hel1
follow Hetzner's documentation and have not been probed. Test buckets:

| Bucket | Set up | it-expectations |
|---|---|---|
| `qbucket-it-test` | console; versioning enabled later (`rclone backend versioning … Enabled`) | `hetzner.json` |
| `qbucket-it-lock` | `rclone mkdir --s3-bucket-object-lock-enabled` (object lock ⇒ versioning) | `hetzner-lock.json` |

Both hold `guard/DO-NOT-DELETE.txt` (see "Writes to unsupported
subresources" below). Versioning and object lock work as documented;
object lock can only be enabled at creation.

## Basics

| | |
|---|---|
| Locations | `fsn1` (Falkenstein), `nbg1` (Nuremberg), `hel1` (Helsinki) |
| Endpoint | `https://<location>.your-objectstorage.com` |
| Signing region | the location (`nbg1`) — no `us-east-1` fallback |
| Addressing | virtual-hosted (`<bucket>.<location>.your-objectstorage.com`); dotted names fall back to path-style |
| LocationConstraint | the location |
| Software | Ceph RGW (request ids `tx…-nbg1-prod1-ceph5`) behind a front proxy (32-hex request ids on what it answers itself); no `Server` header |
| Limits (docs) | 5 TB per object, 5 GB per single PUT and per part, 10 000 parts |

## Capabilities (read-only probes, nbg1)

| Feature | State | Answer |
|---|---|---|
| bucket.versioning | Supported | 200 |
| bucket.object-lock | Supported | 404 ObjectLockConfigurationNotFoundError |
| bucket.lifecycle | Supported | 404 NoSuchLifecycleConfiguration (docs: only some rules) |
| bucket.cors | Supported | 404 NoSuchCORSConfiguration |
| bucket.policy / policy-status | Supported | 404 NoSuchBucketPolicy / 200 |
| bucket.acl, object.acl | Supported | 200 |
| bucket.public-access-block | Supported | 404 NoSuchPublicAccessBlockConfiguration |
| bucket.multipart-listing, versions-listing | Supported | 200 |
| object.tagging | Supported | 200 |
| bucket.tagging | Unsupported | 501 NotImplemented |
| bucket.website | Unsupported | 405 MethodNotAllowed |
| bucket.replication, logging, notifications, ownership-controls | Unsupported | 501 |
| **bucket.encryption** | **Unsupported (profile override)** | GET: 404 …NotFoundError from RGW — but writes are intercepted, see below |
| object.legal-hold, object.retention | Supported on `qbucket-it-lock` (404 ObjectLockConfigurationNotFoundError — RGW's code where AWS says NoSuchObjectLockConfiguration); Unknown on a bucket without object lock (400 "bucket object lock not configured") | |

Profile knowledge for what read probes can't see: SSE-C is the only
encryption mode (Hetzner's "supported actions" page, not yet verified by
writing); batch delete works (integration test). **Copying between
buckets works**, although the same page says CopyObject works within one
bucket only: `qbucket-it-test` → `qbucket-it-lock` (nbg1, one project,
2026-10-20) copied and read back intact — `it_s3_backend crossBucketCopy`
with `QBUCKET_IT_SECOND_BUCKET`, expected in `it-expectations/hetzner*.json`
(`"crossBucketCopy": true`), so a change shows up. `CrossBucketCopy` is
Supported in the profile; a refused copy would only be an error, never a
deleted source.

Multipart uploads (`it_s3_backend`, nbg1, 2026-10-20):

- Create / UploadPart / ListParts / Complete / Abort / ListMultipartUploads
  work; parts are streamed with `UNSIGNED-PAYLOAD` over HTTPS and
  `Content-MD5`; part ETags are the part MD5s and the object ETag is the
  usual MD5-of-MD5s with `-N`.
- `HEAD` / `GET ?partNumber=N` answer the part's size and
  `x-amz-mp-parts-count` — downloads can verify multipart objects part by
  part.
- **The query string survives:** a multipart upload, a part and an abort on
  the key of an existing object left that object untouched
  (`multipartRequestsLeaveAnExistingObjectAlone`). Without it, UploadPart
  would be a PUT of the object and Abort a DELETE — the proxy's
  subresource behaviour (below) made this worth checking first.
- **`If-None-Match: *` is ignored:** a PutObject with it overwrote the
  existing object. The profile knows `ConditionalWrites` as Unsupported, so
  the library refuses the header locally and uploads check for an existing
  target with HEAD (a small race window, documented in the app).
- UploadPartCopy with byte ranges and `x-amz-copy-source-if-match` works
  (`multipartCopy`) — renames above 5 GiB copy in parts.
- **Kill / resume** (devlog 006): a 2 GiB `s3cp` upload (256 parts) killed
  at 45 % and a download killed at 40 % both resumed and verified (MD5 of
  MD5s, copy == source); nothing stayed open. Upload speed equalled a plain
  `curl` PUT from the same line (≈ 2 MiB/s uplink), so the line is the
  limit. `it_operations` uploads a folder (one file multipart) and
  downloads it back, MD5-verified.

Writes verified by the app's own jobs (`tests/it_operations`, both
buckets, 2026-10-20): creating a folder marker, deleting a folder
recursively with DeleteObjects batches (versioned bucket: delete markers),
renaming an object and a folder (CopyObject with
`x-amz-copy-source-if-match`, HEAD, DeleteObject). A stale if-match ETag is
answered with 412 PreconditionFailed, as on AWS.

## Quirks

### Sporadic 403 AccessDenied

Shortly after the bucket was created, 10–30 % of identical, valid requests
got `403 AccessDenied` with an **empty** `<Message>` (HEAD: a bare 403),
across all operations — also with an independent signer, so not a client
bug. No incident was reported for nbg1. Measurements (bucket created
09:13 UTC):

| Time (UTC) | Requests | 403 |
|---|---|---|
| 09:19 | 30 | 3 |
| 09:19 | 40 | 12 |
| 09:20 | 60 | 12 |
| 09:25 | 60 | 6 |
| 09:35 | 100 | 0 |

So it is a start-up effect of a **fresh bucket** (permissions still
spreading through the cluster) that settled within ~20 minutes — but a
real one: a user who creates a bucket in the Hetzner console and opens it
in QBucket right away runs into it.

`HetznerProfile::mapError` marks exactly this shape (`AccessDenied`, empty
message) **retryable**: idempotent requests are retried with backoff; a
genuine denial fails again and stays `NotPermitted`. Denials with a message
and signature errors are not retried.

### Stale reads after switching versioning on

For about 15 minutes after versioning was enabled on `qbucket-it-test`, a
HEAD right after a successful `DELETE` (204) still found the object in
roughly one of three round trips, and listings still showed batch-deleted
keys. 10/10 round trips were clean at 09:56 UTC; the bucket created with
versioning from the start never showed it. Same pattern as the 403s: bucket
state changes need a while to reach every gateway.

Consequence for the verify step of plan → execute → verify (PLAN.md §12.2,
M2/M3): a verification that contradicts a successful response is re-checked
with a delay before it is reported as a failure — and if it persists, it is
reported, never assumed away.

### Writes to unsupported subresources become bucket operations

`PUT /?encryption` (default SSE-S3 encryption) was answered by the front
proxy with `400 InvalidArgument`. **`DELETE /?encryption` was answered
`409 BucketNotEmpty` — the proxy executed it as DeleteBucket.** The test
bucket survived only because it contained objects.

Consequences:

- A read probe proves only the read path. `HetznerProfile::reviewProbe`
  overrides `bucket.encryption` to Unsupported, so the library refuses
  such requests locally.
- **Rule for M6 (bucket management):** a DELETE or PUT on a bucket
  subresource is only sent for features whose *write* support is confirmed
  — by the profile or by a deep probe — never on the strength of a read
  probe. Deep/write probes always run on a bucket that holds a guard object,
  so a misinterpreted DeleteBucket fails with BucketNotEmpty instead of
  deleting the bucket.
- Which other subresource writes the proxy intercepts is not known yet;
  find out (with a guarded scratch bucket) before M6 implements them.

## Open

- Probe fsn1 and hel1.
- Verify SSE-C by writing (scratch bucket, guard object).
- Check whether a fresh *access key* shows the same start-up 403s.
