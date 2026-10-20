#pragma once

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

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

struct PutObjectParams {
    QString bucket;
    QString key;
    QByteArray data;                      // sent with Content-MD5
    QString contentType;                  // empty: application/octet-stream
    QMap<QString, QString> metadata;      // x-amz-meta-*; printable ASCII only (for now)
    QString storageClass;                 // empty: server default
};

struct PutObjectResult {
    QString etag;
    QString versionId;                    // versioned buckets only
    QString requestId;                    // x-amz-request-id, for the journal
};

struct DeleteObjectResult {
    QString versionId;                    // version removed, or of the new delete marker
    bool deleteMarker = false;            // a delete marker was created / removed
    QString requestId;                    // x-amz-request-id, for the journal
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
