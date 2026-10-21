#pragma once

#include <qbucket/s3/BucketConfig.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <optional>

// FakeObjectStore — a stateful S3 store behind a FakeS3Server (path-style
// requests): buckets with objects that keep their data, multipart uploads
// (create, parts, complete, abort, list), byte ranges and part numbers on
// GET/HEAD, If-Match / If-None-Match, CopyObject and UploadPartCopy,
// ListObjectsV2, DeleteObject(s), Content-MD5 checks (BadDigest) and
// composite multipart ETags, object tags (?tagging) and canned ACLs (?acl).
// Versioning per bucket (?versioning): versions and delete markers per key,
// ListObjectVersions, versionId on GET / HEAD / DELETE / copy sources /
// tags / ACLs, the "null" version while versioning is off or suspended,
// x-amz-version-id / x-amz-delete-marker headers, locked versions.
// Buckets over HTTP (CreateBucket with object lock, DeleteBucket refusing a
// bucket with versions or uploads), bucket configurations (policy, CORS,
// lifecycle, object lock, ACL, public access block; the read-only kinds as
// seeded), retention and legal hold per version with the GOVERNANCE bypass,
// a bucket's default retention on new versions.
// Any other object subresource on PUT / POST / DELETE answers 501 — never
// an upload or a delete. Faults are scripted per operation, before or after
// the store executed the request.
//
// It installs itself as the server's fallback handler: explicit routes on
// the server still win. Create it after the server and destroy it first.
//
// Thread affinity: the server's thread (every call and every request).
namespace qbucket::s3::testing {

class FakeObjectStore {
public:
    struct Object {
        QByteArray data;              // empty in discard mode
        qint64 size = 0;
        QByteArray md5;               // raw, of the whole content (empty for discarded multipart objects)
        QString etag;                 // with quotes
        QDateTime lastModified;
        QString contentType;
        QString storageClass;
        QMap<QString, QString> metadata;
        // The other stored headers (cache-control, content-disposition,
        // content-encoding, content-language, expires,
        // x-amz-website-redirect-location, the encryption and object lock
        // headers), lower-case names.
        QMap<QString, QString> headers;
        QString acl = QStringLiteral("private");   // canned ACL
        TagSet tags;
        QList<qint64> partSizes;      // multipart objects: the part layout
    };

    // One entry of a key's history, newest first.
    struct Version {
        QString versionId;            // "null" while versioning is off / suspended
        bool deleteMarker = false;
        bool locked = false;          // DELETE ?versionId= answers 403 AccessDenied
        Object object;                // empty for delete markers
    };

    struct Fault {
        QString operation;            // operationName(), e.g. "UploadPart", "CompleteMultipartUpload"
        int partNumber = 0;           // 0: any part / not a part operation
        int skip = 0;                 // let this many matching requests through first
        int times = 1;                // -1: every time
        bool afterExecuting = false;  // execute, then answer `response` (a lost answer)
        FakeResponse response;
    };

    explicit FakeObjectStore(FakeS3Server* server);
    ~FakeObjectStore();
    FakeObjectStore(const FakeObjectStore&) = delete;
    FakeObjectStore& operator=(const FakeObjectStore&) = delete;

    void createBucket(const QString& bucket);
    // Seeds an object (single-part, ETag = MD5). Returns its version id
    // ("null" unless versioning is enabled).
    QString putObject(const QString& bucket, const QString& key, const QByteArray& data,
                      const QString& contentType = QStringLiteral("application/octet-stream"));

    // An object of `size` bytes without data (for copies and listings of
    // huge objects; GET answers 501 for it).
    QString putSyntheticObject(const QString& bucket, const QString& key, qint64 size);

    // Versioning of a bucket: "Enabled", "Suspended" or "" (never enabled —
    // it can't go back to that once set, as on S3).
    void setVersioning(const QString& bucket, const QString& status);
    // MfaDelete shown by GET ?versioning (nothing enforces it).
    void setMfaDelete(const QString& bucket, bool enabled);
    // Seeds a delete of the current object (a delete marker when versioned);
    // returns the marker's version id, empty when nothing was versioned.
    QString deleteObject(const QString& bucket, const QString& key);
    // Object lock on one version: deleting it answers 403 AccessDenied.
    void lockVersion(const QString& bucket, const QString& key, const QString& versionId);
    // Object lock on a bucket (as if created with it): versioning Enabled.
    void setObjectLockEnabled(const QString& bucket);
    // Seeds a configuration document (Acl: the canned ACL's name; a null
    // document removes it).
    void setBucketConfig(const QString& bucket, BucketConfig kind, const QByteArray& document);
    [[nodiscard]] std::optional<QByteArray> bucketConfig(const QString& bucket, BucketConfig kind) const;
    // GET / PUT / DELETE of this kind answer 501.
    void setUnsupportedBucketConfig(BucketConfig kind);
    void setRetention(const QString& bucket, const QString& key, const QString& versionId, ObjectLockMode mode,
                      const QDateTime& until);
    void setLegalHold(const QString& bucket, const QString& key, const QString& versionId, bool on);
    // A proxy that drops bucket subresources (Hetzner's DELETE ?encryption):
    // PUT ?<config> runs as CreateBucket, DELETE ?<config> as DeleteBucket.
    void setInterceptSubresourceWrites(bool intercept);
    [[nodiscard]] bool hasBucket(const QString& bucket) const;

    // Bodies are hashed, not kept: uploads of any size. GET answers 501.
    void setDiscardData(bool discard);
    // Smallest part except the last (EntityTooSmall); S3: 5 MiB.
    void setMinPartSize(qint64 bytes);
    // false: If-None-Match is ignored and the write overwrites — a server
    // without conditional writes.
    void setHonourIfNoneMatch(bool honour);
    // Ranges are ignored (200 with the whole object) — a broken server.
    void setIgnoreRanges(bool ignore);
    // x-amz-metadata-directive: REPLACE is treated as COPY — a server that
    // keeps the old metadata.
    void setIgnoreMetadataReplace(bool ignore);
    // A proxy that drops object subresources: PUT ?tagging / ?acl are
    // executed as PutObject (the body becomes the object), GET as GetObject.
    void setDropObjectSubresources(bool drop);
    // A copy onto itself with REPLACE loses the object's tags (RGW: Ceph,
    // Hetzner).
    void setDropTagsOnReplace(bool drop);

    // Seeds what a test needs on an existing object (no request involved).
    void setObjectHeader(const QString& bucket, const QString& key, const QString& name, const QString& value);
    void setObjectAcl(const QString& bucket, const QString& key, const QString& cannedAcl);
    void setObjectTags(const QString& bucket, const QString& key, const TagSet& tags);
    void setObjectStorageClass(const QString& bucket, const QString& key, const QString& storageClass);

    void addFault(const Fault& fault);
    void clearFaults();
    // Answers of this operation wait `ms` (after the store executed them).
    void setDelay(const QString& operation, int ms);

    // The current object (not when the newest entry is a delete marker).
    [[nodiscard]] std::optional<Object> object(const QString& bucket, const QString& key) const;
    // Keys with a current object.
    [[nodiscard]] QStringList keys(const QString& bucket) const;
    // A key's history, newest first (one "null" entry when unversioned).
    [[nodiscard]] QList<Version> versions(const QString& bucket, const QString& key) const;
    // Every key with any history (also those whose newest entry is a marker).
    [[nodiscard]] QStringList versionedKeys(const QString& bucket) const;
    [[nodiscard]] QString versioning(const QString& bucket) const;
    [[nodiscard]] QStringList uploadIds(const QString& bucket) const;
    // Parts of an unfinished upload: number → size.
    [[nodiscard]] QMap<int, qint64> uploadParts(const QString& uploadId) const;
    // Every UploadPart / UploadPartCopy that was executed: (uploadId, part number).
    [[nodiscard]] QList<QPair<QString, int>> partUploads() const;
    // Requests the store saw, per operation name (faulted ones included).
    [[nodiscard]] int count(const QString& operation) const;

    // MD5-of-MD5s ETag ("<hex>-<n>", with quotes) of raw part MD5s.
    [[nodiscard]] static QString compositeEtag(const QList<QByteArray>& partMd5s);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qbucket::s3::testing
