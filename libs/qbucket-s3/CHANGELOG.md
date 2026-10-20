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
