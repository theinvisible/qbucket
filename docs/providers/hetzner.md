# Hetzner Object Storage

QBucket's first-class provider (PLAN.md §3.2). Encoded in
`libs/qbucket-s3`: `HetznerProfile` (+ `test_provider_hetzner`), fixtures in
`libs/qbucket-s3/tests/data/hetzner/`, integration expectations in
`libs/qbucket-s3/tests/data/it-expectations/hetzner.json`.

**Verified:** 2026-10-20 against a real project, location **nbg1**, with
`it_s3_backend` (12/12 on each bucket) and `s3ls probe`; fsn1 and hel1
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

Profile knowledge for what read probes can't see (from Hetzner's
"supported actions" page, not yet verified by writing): CopyObject only
within one bucket (`CrossBucketCopy` Unsupported), SSE-C is the only
encryption mode, batch delete works (verified by the integration test).

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
- Verify CrossBucketCopy and SSE-C by writing (scratch bucket, guard object).
- Check whether a fresh *access key* shows the same start-up 403s.
