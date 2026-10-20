#pragma once

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

// Value types exchanged with the library. All of them are plain structs with
// implicitly shared Qt members: cheap to copy across threads, reentrant.

namespace qbucket::s3 {

// --- Buckets -----------------------------------------------------------------

struct BucketInfo {
    QString name;
    QDateTime creationDate;   // UTC
    QString region;           // only if the server reports it in the listing
};

struct BucketList {
    QList<BucketInfo> buckets;
    QString ownerId;
    QString ownerDisplayName;
};

// --- Listing -----------------------------------------------------------------

struct ObjectEntry {
    QString key;              // decoded, exactly as stored
    qint64 size = 0;
    QDateTime lastModified;   // UTC
    QString etag;             // as returned, including the quotes
    QString storageClass;     // empty if not reported
};

struct ListObjectsParams {
    QString bucket;
    QString prefix;
    QString delimiter;        // "/" for folder-style browsing, empty for flat
    QString continuationToken;
    QString startAfter;
    int maxKeys = 1000;
};

// One page of ListObjectsV2.
struct ListPage {
    QString bucket;
    QString prefix;
    QString delimiter;
    QList<ObjectEntry> objects;
    QStringList commonPrefixes;    // decoded, with the trailing delimiter
    bool isTruncated = false;
    QString nextContinuationToken; // set when isTruncated
};

// --- Objects -----------------------------------------------------------------

struct ObjectMeta {
    QString key;
    qint64 size = -1;
    QDateTime lastModified;   // UTC
    QString etag;
    QString contentType;
    QString storageClass;
    QString versionId;
    // x-amz-mp-parts-count: parts of a multipart object, when the server
    // reports it (HEAD/GET with a part number); 0 otherwise.
    int partsCount = 0;
    QMap<QString, QString> userMetadata;  // x-amz-meta-* without the prefix, lower-case names
    // Every response header as received (names as sent, in order) — for
    // showing an object's headers; the fields above are parsed from these.
    QList<std::pair<QString, QString>> responseHeaders;
};

// How far a download's content could be verified. Reported honestly: an
// ETag is only an MD5 for single-part uploads without SSE-KMS / SSE-C.
enum class Integrity : quint8 {
    Md5Verified,     // MD5 of the received bytes equals the ETag
    SizeVerified,    // only the length could be checked
};

struct ObjectData {
    ObjectMeta meta;
    QByteArray data;
    Integrity integrity = Integrity::SizeVerified;
};

// A byte range of a local file as a request body: streamed from disk on the
// worker thread, never held in memory, re-read from the start on a retry.
// The hashes are computed by the caller beforehand (e.g. on a thread pool):
//   - md5Base64 is sent as Content-MD5 and is required — the server rejects a
//     body that changed after hashing (BadDigest);
//   - sha256Hex is the signed payload hash; it is needed only when
//     Session::requiresPayloadHash() (plain HTTP, or a provider that refuses
//     UNSIGNED-PAYLOAD) and ignored otherwise.
struct FileBody {
    QString path;
    qint64 offset = 0;
    qint64 length = 0;
    QByteArray md5Base64;
    QByteArray sha256Hex;
};

struct PutObjectParams {
    QString bucket;
    QString key;
    QByteArray data;                      // sent with Content-MD5
    // Instead of `data` (which must then be empty): stream a file range.
    // Such requests run on the Session's transfer lanes.
    std::optional<FileBody> file;
    QString contentType;                  // empty: application/octet-stream
    QMap<QString, QString> metadata;      // x-amz-meta-*; printable ASCII only (for now)
    QString storageClass;                 // empty: server default
    // If-None-Match: * — the server refuses (PreconditionFailed) when the key
    // exists. Only meaningful where Feature::ConditionalWrites is confirmed:
    // a server that ignores the header overwrites.
    bool ifNoneMatch = false;
};

struct PutObjectResult {
    QString etag;
    QString versionId;                    // versioned buckets only
    QString requestId;                    // x-amz-request-id, for the journal
    // The server throttled (SlowDown / 503) at least once before the request
    // succeeded — a signal for adaptive concurrency.
    bool throttled = false;
};

// GetObject streamed into a local file. The file is created if missing and
// never truncated: the bytes land at fileOffset onwards (several ranges of
// one object can be downloaded into one preallocated file in parallel). A
// retry writes the same range again from its start.
struct GetObjectToFileParams {
    QString bucket;
    QString key;
    QString versionId;                    // empty: the current version
    QString filePath;
    qint64 fileOffset = 0;
    // Byte range of the object; rangeLength < 0: the whole object. Mutually
    // exclusive with partNumber.
    qint64 rangeStart = 0;
    qint64 rangeLength = -1;
    int partNumber = 0;                   // > 0: GET ?partNumber= (one part of a multipart object)
    QString ifMatch;                      // ETag with quotes; the object changed → PreconditionFailed
};

struct DownloadResult {
    ObjectMeta meta;                      // headers of this response (size: bytes of this response)
    qint64 bytesWritten = 0;
    QByteArray md5;                       // raw MD5 (16 bytes) of the bytes written
    qint64 objectSize = -1;               // whole object: from Content-Range or Content-Length
    int partsCount = 0;                   // x-amz-mp-parts-count, if the server sent it
    // Whole single-part objects: the MD5 was compared with the ETag. Ranges
    // and parts are only size-checked here; the caller verifies the whole.
    Integrity integrity = Integrity::SizeVerified;
    bool throttled = false;               // as PutObjectResult::throttled
};

struct DeleteObjectResult {
    QString versionId;                    // version removed, or of the new delete marker
    bool deleteMarker = false;            // a delete marker was created / removed
    QString requestId;                    // x-amz-request-id, for the journal
};

// CopyObject within or between buckets (up to 5 GiB; larger objects need
// UploadPartCopy). Metadata, content type and tags are copied (directive
// COPY); the storage class is NOT — without storageClass the copy lands in
// the server's default class. ACLs are never copied.
struct CopyObjectParams {
    QString sourceBucket;
    QString sourceKey;
    QString sourceVersionId;              // empty: the current version
    QString bucket;                       // destination
    QString key;
    QString ifMatch;                      // x-amz-copy-source-if-match, ETag with quotes; empty: none
    QString storageClass;                 // of the copy; empty: server default
};

struct CopyObjectResult {
    QString etag;
    QDateTime lastModified;
    QString versionId;                    // of the copy (versioned buckets)
    QString sourceVersionId;
    QString requestId;                    // x-amz-request-id, for the journal
};

// HeadObject with options.
struct HeadObjectParams {
    QString bucket;
    QString key;
    QString versionId;
    int partNumber = 0;                   // > 0: size of that part, and ObjectMeta::partsCount
    QString ifMatch;
};

// --- Multipart uploads ---------------------------------------------------------

struct CreateMultipartUploadParams {
    QString bucket;
    QString key;
    QString contentType;                  // empty: application/octet-stream
    QMap<QString, QString> metadata;      // as PutObjectParams::metadata
    QString storageClass;
};

struct CreateMultipartUploadResult {
    QString bucket;
    QString key;
    QString uploadId;
    QString requestId;
};

// One part, streamed from a file range with its Content-MD5 (FileBody).
// Uploading a part number again replaces it, so retries are safe.
struct UploadPartParams {
    QString bucket;
    QString key;
    QString uploadId;
    int partNumber = 0;                   // 1..10000
    FileBody file;
};

struct UploadPartResult {
    int partNumber = 0;
    QString etag;                         // as returned, with quotes
    QString requestId;
    bool throttled = false;               // as PutObjectResult::throttled
};

struct CompletedPart {
    int partNumber = 0;
    QString etag;                         // with quotes
};

struct CompleteMultipartUploadParams {
    QString bucket;
    QString key;
    QString uploadId;
    QList<CompletedPart> parts;           // ascending part numbers
    bool ifNoneMatch = false;             // as PutObjectParams::ifNoneMatch
};

struct CompleteMultipartUploadResult {
    QString etag;                         // "<md5 of the part MD5s>-<parts>" on S3
    QString location;
    QString versionId;
    QString requestId;
};

struct ListPartsParams {
    QString bucket;
    QString key;
    QString uploadId;
    int partNumberMarker = 0;             // parts after this one
    int maxParts = 1000;
};

struct PartInfo {
    int partNumber = 0;
    QString etag;
    qint64 size = 0;
    QDateTime lastModified;
};

struct ListPartsPage {
    QList<PartInfo> parts;
    bool isTruncated = false;
    int nextPartNumberMarker = 0;
    QString storageClass;
};

struct ListMultipartUploadsParams {
    QString bucket;
    QString prefix;
    QString keyMarker;
    QString uploadIdMarker;
    int maxUploads = 1000;
};

struct MultipartUploadInfo {
    QString key;                          // decoded
    QString uploadId;
    QDateTime initiated;
    QString storageClass;
    QString initiator;                    // display name or id, if reported
};

struct ListMultipartUploadsPage {
    QList<MultipartUploadInfo> uploads;
    bool isTruncated = false;
    QString nextKeyMarker;
    QString nextUploadIdMarker;
};

// One part of a multipart upload copied server-side from (a byte range of)
// an existing object — for copies above 5 GiB. Replacing a part is
// harmless, so it is retried like UploadPart; a 200 with an <Error> body is
// an error.
struct UploadPartCopyParams {
    QString sourceBucket;
    QString sourceKey;
    QString sourceVersionId;
    QString bucket;                       // destination upload
    QString key;
    QString uploadId;
    int partNumber = 0;
    qint64 rangeStart = 0;                // byte range of the source;
    qint64 rangeLength = -1;              // < 0: the whole source
    QString ifMatch;                      // x-amz-copy-source-if-match
};

struct UploadPartCopyResult {
    int partNumber = 0;
    QString etag;
    QDateTime lastModified;
    QString requestId;
    bool throttled = false;
};

struct CreateBucketParams {
    QString bucket;
    QString region;                       // → LocationConstraint via the provider profile
    bool objectLockEnabled = false;       // only possible at creation
};

struct ObjectIdentifier {
    QString key;
    QString versionId;        // empty: current version
};

struct DeleteObjectsResult {
    struct Deleted {
        QString key;
        QString versionId;
        bool deleteMarker = false;
        QString deleteMarkerVersionId;
    };
    struct Failed {
        QString key;
        QString versionId;
        QString code;
        QString message;
    };
    QList<Deleted> deleted;
    QList<Failed> failed;
    QString requestId;        // x-amz-request-id of the batch, for the journal
};

} // namespace qbucket::s3
