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
- `Session::copyObject` with `CopyObjectParams` / `CopyObjectResult` and
  `Operation::CopyObject`: server-side copy up to 5 GiB, metadata directive
  COPY, optional `x-amz-copy-source-if-match` and storage class; a 200 with
  an `<Error>` body is an error; never retried after a lost answer.

- `AwsProfile` and `GarageProfile` know `BatchDelete` as supported (AWS also
  `CrossBucketCopy`): write-only features the probes cannot confirm.

- Streaming transfers: `FileBody` (a file range as a request body, read from
  disk on every attempt, with caller-computed Content-MD5 and optional
  SHA-256) in `PutObjectParams::file`; `Session::getObjectToFile` with
  `GetObjectToFileParams` (whole object, byte range or part number, written
  at a file offset, `If-Match`) and `DownloadResult` (bytes, MD5, object
  size, parts count, integrity). A server that ignores the Range header is an
  error.
- `ProgressCallback` for `putObject` and `getObjectToFile` (≤ 10 per second
  plus the final value, always before the result).
- `SessionOptions::transferLanes`: worker threads with their own QNAM for
  transfer requests, started on demand; browsing stays on its own thread.
- `Session::requiresPayloadHash()`: whether streamed bodies need their
  SHA-256 (plain HTTP, or a profile that vetoes `UNSIGNED-PAYLOAD`).
- `PutObjectResult::throttled` / `DownloadResult::throttled`: the server
  answered SlowDown / 503 before the request succeeded.
- `ErrorCategory::LocalIo` — a local file could not be read or written
  (never retried).
- Multipart uploads on `Session`: `createMultipartUpload`, `uploadPart`
  (streamed `FileBody`, on a transfer lane), `completeMultipartUpload`
  (200-with-error aware, never retried after a lost answer),
  `abortMultipartUpload`, `listParts`, `listMultipartUploads` (both paged
  with loop detection), `uploadPartCopy` (byte ranges, copy-source-if-match);
  the value types in `Types.h`, `Operation` values appended,
  `Feature::MultipartUpload` (profile knowledge, Supported by default).
- `headObject(HeadObjectParams)` with a part number / If-Match;
  `ObjectMeta::partsCount` (`x-amz-mp-parts-count`).
- `PutObjectParams::ifNoneMatch` / `CompleteMultipartUploadParams::ifNoneMatch`
  (`If-None-Match: *`); refused locally where `ConditionalWrites` is
  Unsupported. The short-circuit also checks `CrossBucketCopy` for copies
  between buckets.
- `HetznerProfile` and `GarageProfile` know `ConditionalWrites` as
  Unsupported: both ignored `If-None-Match` and overwrote the object.
- `testing::FakeObjectStore`: a stateful S3 store behind `FakeS3Server` —
  objects with data, multipart, ranges, part numbers, conditional writes,
  copies, listings, scripted faults, a discard mode for huge uploads.
  `FakeS3Server::setRecordBodies()`.
- `qbucket::s3_transfer` (Qt Concurrent), namespace `qbucket::s3::transfer`:
  `planParts` / `PartPlan` (64 MiB threshold, parts of at least 8 MiB in
  whole MiB, within the profile's limits, at most 5 TiB), `hashRange` /
  `hashRangeNow` (MD5 + optional SHA-256 of a file range on a thread pool,
  cancellable), ETag helpers (`md5Etag`, `compositeEtag`, `etagPartCount`,
  `sameEtag`), `AdaptiveConcurrency` (AIMD, one decrease per window),
  `ResumeStore` with `TransferRecord` / `PartRecord`, and
  `JsonFileResumeStore` (atomic record, appended part lines, survives a torn
  last line).
- `transfer::Uploader`: one file as a single PUT or a multipart upload with
  parts in parallel under a shared `AdaptiveConcurrency`, hashed a part
  ahead, Content-MD5 on every body, retries of failed parts, pause / resume /
  cancel (abort, or keep for resume), a lost Complete answer resolved with
  HEAD, ETag verification (MD5 / MD5-of-MD5s, else size), a source that
  changes during the upload fails it; with a `ResumeStore` every finished
  part is recorded and `Uploader::fromStore` continues after a crash
  (ListParts decides; unrecorded parts are adopted when the local bytes hash
  to their ETag; a vanished upload starts over). `flushResumeWrites()`.
  `TransferRecord` gained content type, storage class, metadata and
  If-None-Match. Logging category `qbucket.s3.transfer`.
- `transfer::Downloader`: one object into `<target>.qbpart` — a single GET
  or ranges in parallel (the object's own part layout from HEAD
  `?partNumber=1` where it has one), every request with If-Match on the
  ETag and the version pinned — then verified (MD5 for single-part objects,
  also over ranges; MD5-of-MD5s for multipart ones; else honestly size
  only), renamed onto the target atomically (an existing one only with
  `overwrite`), mtime from Last-Modified. A failed check keeps the .qbpart.
  With a `ResumeStore` `Downloader::fromStore` continues after a crash:
  same ETag or start over, recorded ranges re-hashed from disk first.
- `GetObjectToFileParams::createFile` (false: the file must exist).
- `transfer::Copier`: server-side copy of any size with UploadPartCopy —
  the source's content type, metadata and storage class carried over,
  ranges in parallel, each with `x-amz-copy-source-if-match`, the version
  pinned, a lost Complete resolved with HEAD, the copy's size checked, the
  upload aborted on failure. Verified against Ceph RGW, VersityGW, Garage and
  Hetzner (`it_s3_backend multipartCopy`).
- `AdaptiveConcurrency::setMaximum()`.
- `examples/s3cp` — resumable uploads and downloads from the command line.
- `FakeObjectStore::setDelay()`.
- `SessionOptions::serverWorkTimeoutMs` (default 15 min): the transfer timeout
  of CompleteMultipartUpload, CopyObject and UploadPartCopy, which the server
  works on before it sends a byte. With the 120 s default a 50 GiB Complete
  against VersityGW timed out, the server cancelled the assembly, and the
  upload stayed open.
- `ObjectHeaders` (content type, Cache-Control, Content-Disposition,
  Content-Encoding, Content-Language, Expires, website redirect, user
  metadata) and `objectHeadersOf(ObjectMeta)`; `ObjectMeta` reports those
  headers, the encryption (`serverSideEncryption`, `sseKmsKeyId`,
  `sseCustomerAlgorithm`), `tagCount` and the object lock
  (`objectLockMode`, `objectLockRetainUntil`, `legalHold`).
- `CopyObjectParams::metadataDirective` (`MetadataDirective::Replace` sends
  `headers` in full), `serverSideEncryption`, `cannedAcl`. A copy onto
  itself is allowed when it changes something (Replace, a storage class, an
  encryption).
- `ProviderProfile::storageClasses()` (AWS: the seven classes; Hetzner and
  Garage: STANDARD; generic: unknown) and `isArchiveStorageClass()`.
- `FakeObjectStore` keeps the stored headers and a canned ACL, honours the
  metadata directive, refuses an unchanged copy onto itself like AWS;
  `setIgnoreMetadataReplace()`.

- Object tags and ACLs: `Session::getObjectTagging` / `putObjectTagging`
  (`TagSet`, `tagSetProblem()`; an empty set removes all tags — there is no
  DeleteObjectTagging) and `getObjectAcl` / `putObjectAcl` (canned ACL;
  `AccessControlPolicy`, `Grant`, `cannedAclOf()`); `Operation` values
  `GetObjectTagging`, `PutObjectTagging`, `GetObjectAcl`, `PutObjectAcl`.
- Write-only features `Feature::ObjectTaggingWrite` ("object.tagging-write")
  and `Feature::ObjectAclWrite` ("object.acl-write"): never set by a read
  probe. Required by the tag / ACL writes and by `x-amz-tagging` /
  `x-amz-acl` on object writes (local short-circuit when Unsupported).
  Profiles: AWS Supported, Garage Unsupported, generic Unknown.
- `featuresFor()` lists the operation's own feature first; a 501 teaches
  only that one (a refused PutObjectTagging no longer hides reading tags).
- A PutObjectTagging / PutObjectAcl answered with an ETag (an object
  upload — what a proxy dropping the subresource would do) is a Protocol
  error, never a success.
- `CopyObjectParams::tags` (tagging directive REPLACE); otherwise the copy
  sends the directive COPY explicitly.
- `ProviderProfile::cannedObjectAcls()` (Hetzner: private, public-read).
- XML: `parseTagging`, `buildTagging`, `parseAccessControlPolicy`.
- `FakeObjectStore`: tags and canned ACLs (GET/PUT `?tagging` / `?acl`,
  `x-amz-tagging`, tagging directive, `x-amz-tagging-count`); any other object
  subresource on PUT / POST / DELETE answers 501 instead of falling through
  to an upload or a delete; `setDropObjectSubresources()` simulates a proxy
  that drops them.
- `it_s3_backend`: canary and victim tests for tag / ACL writes
  (`subresourceWritesOnMissingKeyCreateNothing`,
  `objectSubresourceWritesLeaveDataAlone`), `copyKeepsTags`,
  `copyResetsAclUnlessCanned`; `QBUCKET_IT_TRACE_BODIES=1` prints response
  bodies.

### Changed

- `CreateMultipartUploadParams`: `contentType` and `metadata` are replaced
  by `headers` (`ObjectHeaders`); new `serverSideEncryption`, `cannedAcl`,
  `tagging`.
- `transfer::Copier` carries all of the source's headers (Cache-Control,
  Content-Disposition, …), not only content type and user metadata.

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
