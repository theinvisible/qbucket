#pragma once

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
// composite multipart ETags. Faults are scripted per operation, before or
// after the store executed the request.
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
        QList<qint64> partSizes;      // multipart objects: the part layout
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
    // Seeds an object (single-part, ETag = MD5).
    void putObject(const QString& bucket, const QString& key, const QByteArray& data,
                   const QString& contentType = QStringLiteral("application/octet-stream"));

    // Bodies are hashed, not kept: uploads of any size. GET answers 501.
    void setDiscardData(bool discard);
    // Smallest part except the last (EntityTooSmall); S3: 5 MiB.
    void setMinPartSize(qint64 bytes);
    // false: If-None-Match is ignored and the write overwrites — a server
    // without conditional writes.
    void setHonourIfNoneMatch(bool honour);
    // Ranges are ignored (200 with the whole object) — a broken server.
    void setIgnoreRanges(bool ignore);

    void addFault(const Fault& fault);
    void clearFaults();
    // Answers of this operation wait `ms` (after the store executed them).
    void setDelay(const QString& operation, int ms);

    [[nodiscard]] std::optional<Object> object(const QString& bucket, const QString& key) const;
    [[nodiscard]] QStringList keys(const QString& bucket) const;
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
