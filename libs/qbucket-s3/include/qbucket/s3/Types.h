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

// --- Versions ----------------------------------------------------------------

// One entry of ListObjectVersions: a version of an object or a delete marker.
// The server sorts by key, then newest first; isLatest marks the current one.
struct VersionEntry {
    QString key;              // decoded
    QString versionId;        // "null" for the version written while versioning was off
    bool isLatest = false;
    bool isDeleteMarker = false;
    qint64 size = 0;          // 0 for delete markers
    QDateTime lastModified;   // UTC
    QString etag;             // quoted; empty for delete markers
    QString storageClass;     // empty for delete markers or if not reported
};

struct ListObjectVersionsParams {
    QString bucket;
    QString prefix;
    QString delimiter;        // "/" for folder-style browsing, empty for flat
    QString keyMarker;
    QString versionIdMarker;  // only together with keyMarker
    int maxKeys = 1000;
};

// One page of ListObjectVersions. Versions and delete markers in the order
// the server sent them (key, then newest first) — a key's versions may
// continue on the next page.
struct ListObjectVersionsPage {
    QString bucket;
    QString prefix;
    QString delimiter;
    QList<VersionEntry> entries;
    QStringList commonPrefixes;    // decoded, with the trailing delimiter
    bool isTruncated = false;
    QString nextKeyMarker;         // set when isTruncated
    QString nextVersionIdMarker;
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
    // The other headers stored with the object (raw values; empty: not set).
    QString cacheControl;
    QString contentDisposition;
    QString contentEncoding;
    QString contentLanguage;
    QString expires;                      // as sent, not parsed
    QString websiteRedirectLocation;      // x-amz-website-redirect-location
    // Encryption: x-amz-server-side-encryption ("AES256", "aws:kms", …), the
    // KMS key, and SSE-C's algorithm (such objects need the customer's key).
    QString serverSideEncryption;
    QString sseKmsKeyId;
    QString sseCustomerAlgorithm;
    int tagCount = -1;                    // x-amz-tagging-count; -1: not reported
    // Object lock on this version (only reported with the permission to read it).
    QString objectLockMode;               // GOVERNANCE / COMPLIANCE; empty: none
    QDateTime objectLockRetainUntil;      // UTC
    bool legalHold = false;
    // Every response header as received (names as sent, in order) — for
    // showing an object's headers; the fields above are parsed from these.
    QList<std::pair<QString, QString>> responseHeaders;
};

// The headers of an object a client sets — what a copy with
// MetadataDirective::Replace sends in full: anything left empty here is
// gone afterwards. Values must be printable ASCII.
struct ObjectHeaders {
    QString contentType;                  // empty: application/octet-stream
    QString cacheControl;
    QString contentDisposition;
    QString contentEncoding;
    QString contentLanguage;
    QString expires;                      // an HTTP date, sent as given
    QString websiteRedirectLocation;
    QMap<QString, QString> userMetadata;  // x-amz-meta-*, lower-case names
    bool operator==(const ObjectHeaders&) const = default;
};

// The settable headers of an object as HEAD reported them.
inline ObjectHeaders objectHeadersOf(const ObjectMeta& meta)
{
    ObjectHeaders h;
    h.contentType = meta.contentType;
    h.cacheControl = meta.cacheControl;
    h.contentDisposition = meta.contentDisposition;
    h.contentEncoding = meta.contentEncoding;
    h.contentLanguage = meta.contentLanguage;
    h.expires = meta.expires;
    h.websiteRedirectLocation = meta.websiteRedirectLocation;
    h.userMetadata = meta.userMetadata;
    return h;
}

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

// GetObject streamed into a local file. The file is never truncated: the
// bytes land at fileOffset onwards (several ranges of one object can be
// downloaded into one preallocated file in parallel). A retry writes the
// same range again from its start.
struct GetObjectToFileParams {
    QString bucket;
    QString key;
    QString versionId;                    // empty: the current version
    QString filePath;
    qint64 fileOffset = 0;
    // false: the file must exist already (a LocalIo error otherwise) — a
    // request still under way never re-creates a file its caller removed.
    bool createFile = true;
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

struct Tag {
    QString key;
    QString value;
    bool operator==(const Tag&) const = default;
};
using TagSet = QList<Tag>;

// Where a copy's metadata and headers come from.
enum class MetadataDirective : quint8 {
    Copy,      // from the source (content type, headers, user metadata)
    Replace,   // exactly CopyObjectParams::headers — everything else is dropped
};

// CopyObject within or between buckets (up to 5 GiB; larger objects need
// UploadPartCopy). By default metadata, headers and tags are copied
// (directive COPY); the storage class is NOT — without storageClass the copy
// lands in the server's default class. ACLs are never copied: the copy gets
// cannedAcl, or the server's default (usually private).
struct CopyObjectParams {
    QString sourceBucket;
    QString sourceKey;
    QString sourceVersionId;              // empty: the current version
    QString bucket;                       // destination
    QString key;
    QString ifMatch;                      // x-amz-copy-source-if-match, ETag with quotes; empty: none
    QString storageClass;                 // of the copy; empty: server default
    // Replace: `headers` are the copy's headers in full. A copy onto itself
    // (same bucket and key, no sourceVersionId) must change something:
    // Replace, a storageClass or an encryption.
    MetadataDirective metadataDirective = MetadataDirective::Copy;
    ObjectHeaders headers;                // with Replace only
    QString serverSideEncryption;         // x-amz-server-side-encryption ("AES256"); empty: server default
    QString cannedAcl;                    // x-amz-acl; empty: none
    // Set: the copy gets exactly these tags (x-amz-tagging-directive
    // REPLACE; Feature::ObjectTaggingWrite). Unset: the source's tags
    // (directive COPY). Not reliable beyond AWS: RGW drops an object's tags
    // on a REPLACE copy onto itself whatever the directive, and Hetzner's
    // leaves tags sent this way unreadable — write them with
    // PutObjectTagging after the copy instead (it_s3_backend copyKeepsTags).
    std::optional<TagSet> tags;
};

struct CopyObjectResult {
    QString etag;
    QDateTime lastModified;
    QString versionId;                    // of the copy (versioned buckets)
    QString sourceVersionId;
    QString requestId;                    // x-amz-request-id, for the journal
};

// --- Tags and ACLs ------------------------------------------------------------

// S3's rules for an object's tags: at most 10, keys of 1–128 and values of up
// to 256 characters, keys unique and not starting with "aws:". Empty when
// the set is valid, otherwise what is wrong (English).
inline QString tagSetProblem(const TagSet& tags)
{
    if (tags.size() > 10)
        return QStringLiteral("an object has at most 10 tags");
    QStringList seen;
    for (const Tag& t : tags) {
        if (t.key.isEmpty() || t.key.size() > 128)
            return QStringLiteral("tag keys have 1 to 128 characters");
        if (t.value.size() > 256)
            return QStringLiteral("tag values have at most 256 characters");
        if (t.key.startsWith(QLatin1String("aws:"), Qt::CaseInsensitive))
            return QStringLiteral("tag keys must not start with \"aws:\"");
        if (seen.contains(t.key))
            return QStringLiteral("tag key \"%1\" appears twice").arg(t.key);
        seen.append(t.key);
    }
    return {};
}

struct ObjectTagging {
    TagSet tags;
    QString versionId;                    // the version the tags belong to (versioned buckets)
};

struct PutObjectTaggingParams {
    QString bucket;
    QString key;
    QString versionId;                    // empty: the current version
    TagSet tags;                          // the complete new set; empty removes all tags
};

struct PutObjectAclParams {
    QString bucket;
    QString key;
    QString versionId;                    // empty: the current version
    QString cannedAcl;                    // x-amz-acl, e.g. "private", "public-read"
};

// Answer of PutObjectTagging / PutObjectAcl.
struct SubresourceWriteResult {
    QString versionId;
    QString requestId;
};

struct Grant {
    enum class Grantee : quint8 { CanonicalUser, Group, Email };
    Grantee grantee = Grantee::CanonicalUser;
    QString id;                           // CanonicalUser
    QString displayName;
    QString uri;                          // Group, e.g. http://acs.amazonaws.com/groups/global/AllUsers
    QString email;                        // Email (AmazonCustomerByEmail)
    QString permission;                   // FULL_CONTROL, READ, WRITE, READ_ACP, WRITE_ACP
};

struct AccessControlPolicy {
    QString ownerId;
    QString ownerName;
    QList<Grant> grants;
};

// The canned ACL a policy amounts to: "private", "public-read",
// "public-read-write" or "authenticated-read" (the owner with FULL_CONTROL
// plus exactly the canned groups). nullopt for anything else — grants a
// canned ACL can't express, which rewriting the ACL would lose.
inline std::optional<QString> cannedAclOf(const AccessControlPolicy& acl)
{
    static const QString allUsers = QStringLiteral("http://acs.amazonaws.com/groups/global/AllUsers");
    static const QString authUsers = QStringLiteral("http://acs.amazonaws.com/groups/global/AuthenticatedUsers");
    bool ownerFull = false;
    QStringList others;   // "<group>:<permission>"
    for (const Grant& g : acl.grants) {
        if (g.grantee == Grant::Grantee::CanonicalUser && g.id == acl.ownerId && !acl.ownerId.isEmpty()) {
            if (g.permission == QLatin1String("FULL_CONTROL"))
                ownerFull = true;
            else
                return std::nullopt;
        } else if (g.grantee == Grant::Grantee::Group && (g.uri == allUsers || g.uri == authUsers)) {
            others.append((g.uri == allUsers ? QStringLiteral("all:") : QStringLiteral("auth:")) + g.permission);
        } else {
            return std::nullopt;
        }
    }
    if (!ownerFull)
        return std::nullopt;
    others.sort();
    others.removeDuplicates();
    if (others.isEmpty())
        return QStringLiteral("private");
    if (others == QStringList{QStringLiteral("all:READ")})
        return QStringLiteral("public-read");
    if (others == (QStringList{QStringLiteral("all:READ"), QStringLiteral("all:WRITE")}))
        return QStringLiteral("public-read-write");
    if (others == QStringList{QStringLiteral("auth:READ")})
        return QStringLiteral("authenticated-read");
    return std::nullopt;
}

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
    ObjectHeaders headers;                // content type, headers, user metadata of the object
    QString storageClass;
    QString serverSideEncryption;         // as CopyObjectParams::serverSideEncryption
    QString cannedAcl;                    // x-amz-acl; empty: none
    QString tagging;                      // x-amz-tagging (URL query form "k=v&k2=v2"); empty: none
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
