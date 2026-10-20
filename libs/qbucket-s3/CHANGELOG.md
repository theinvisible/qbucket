# Changelog — qbucket-s3

All notable changes to the library's public API. The library has its own
semantic version, independent of the QBucket application. Before 1.0, a minor
version bump may break the API.

## [Unreleased] — 0.1.0

### Added

- Library skeleton: standalone CMake project, `qbucket::s3` target (static by
  default, `QBUCKET_S3_BUILD_SHARED` for shared), export header, installed
  CMake package `QBucketS3`.
- `Result<T>` / `Result<void>` and `Error` with `ErrorCategory`.
- `Version.h` with `QBUCKET_S3_VERSION_*` macros.
- `Endpoint` (URL, signing region, `AddressingStyle`, pinned certificates) and
  the value types in `Types.h` (`BucketInfo`, `ListPage`, `ObjectMeta`, …).
- `Credentials` value type and `Presigner` (SigV4 query signing, at most 7 days).
- Logging categories `qbucket.s3.{http,signing,session,retry,capabilities}`
  (`Logging.h`) and the `RequestObserver` audit hook with `RequestRecord`.
- `qbucket::s3_testsupport` with `testing::FakeS3Server`: scriptable HTTP/1.1
  test server with fault injection and SigV4 verification (header and
  presigned form, clock-skew and payload-hash checks).
- `CredentialsProvider` interface (asynchronous, thread-safe) with
  `StaticCredentialsProvider` and `EnvCredentialsProvider`.
- `Operation` enum, `ProviderProfile` hooks (endpoints, signing region,
  limits, `prepareRequest`, `normalizeListing`, `mapError`) and
  `GenericS3Profile`.
- Value types for object and bucket writes: `ObjectData` with `Integrity`,
  `PutObjectParams` / `PutObjectResult`, `DeleteObjectResult`,
  `CreateBucketParams`; `ProviderProfile::locationConstraintFor()`.

- `Session` — the library's entry point: worker thread with its own
  QNetworkAccessManager; callback + context API (`RequestId`, exactly-once
  queued delivery on the context's thread, dropped when the context dies),
  `cancel()`, `cancelAll()`, non-blocking `shutdown()`; presigning with the
  learned clock offset and region; `clockSkewDetected` signal.
- `ListObjectsPager` — page-by-page listing with loop detection across pages.

- `SessionOptions::initialClockOffsetSecs` (carry a learned offset over).
- `ObjectMeta::responseHeaders`: every header of the HeadObject / GetObject
  answer, as received.
- `requestId` in `PutObjectResult`, `DeleteObjectResult` and
  `DeleteObjectsResult` (`x-amz-request-id`, for audit journals).

### Fixed

- `ServerFingerprint::requestIdShape` no longer contains the length of
  request ids that are neither hex nor RGW-style: ids of a growing counter
  changed the fingerprint with every probe, and `capabilities::merge` then
  dropped all earlier definitive results (a 5xx probe could erase a known
  "Supported").
- `examples/s3ls` — CLI over the public API; also the install-consumer check.
- Integration tests (`it_s3_backend`) with Docker backends (Ceph RGW,
  VersityGW, Garage) and per-backend expectation files for known deviations.

- `ProviderRegistry` with `HetznerProfile` (fsn1/nbg1/hel1, virtual-hosted,
  location = signing region), `AwsProfile`, `GarageProfile` (maps Garage's
  "Invalid signature" AccessDenied to Authentication) and `GenericS3Profile`.
- `AwsSharedFileCredentialsProvider` (~/.aws/credentials format, re-read on
  every fetch).

- Capability safeguard (`Capabilities.h`): `Feature` (stable string ids),
  `CapabilityState`, `CapabilitySet` with fingerprint and bucket state, JSON
  persistence, `capabilities::effective/decide/merge/diff/isExpired`;
  `Session::probeServer/probeBucket` (read-only GET probes, four in
  parallel), `Session::setCapabilities` with a local short-circuit for
  Unsupported features, `capabilityLearned` from real 501s;
  `ProviderProfile::capabilities()` and `reviewProbe()`.

- `Error::retryable` — a provider profile marks a failure as worth retrying
  (idempotent requests only, category unchanged). `HetznerProfile` uses it
  for Hetzner's sporadic empty-message 403s and overrides the misleading
  `?encryption` read probe.

- `Result<T>::error()` on a success no longer is undefined behaviour in
  release builds (asserts in debug, returns an Internal error otherwise);
  `error() &&` returns by value.

### Safety

- Retries only where safe: transient failures of idempotent requests, and
  non-idempotent ones only when they never reached the server; exponential
  backoff with full jitter, `Retry-After` honoured.
- Clock skew is corrected from `<ServerTime>` / `Date` (one re-sign, also for
  non-idempotent requests, which the server rejected unexecuted); a learned
  offset is kept for later requests.
- Region redirects are resolved from `x-amz-bucket-region` / `<Region>` and
  cached per bucket; a `Location` header is never followed.

- Object keys with `.` / `..` path segments are refused with
  `InvalidRequest`: Qt's network stack normalises them away on the wire, so
  the request would address a different object.
