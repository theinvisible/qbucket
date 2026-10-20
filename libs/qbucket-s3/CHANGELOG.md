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
