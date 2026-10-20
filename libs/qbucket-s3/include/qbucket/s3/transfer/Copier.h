#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/transfer/Export.h>

#include <QObject>
#include <QString>

#include <memory>

namespace qbucket::s3::transfer {

class AdaptiveConcurrency;

struct CopyRequest {
    QString sourceBucket;
    QString sourceKey;
    QString sourceVersionId;
    QString bucket;                       // destination
    QString key;
    // The source ETag the caller planned with; a changed source fails the
    // copy (PreconditionFailed). Empty: the ETag HEAD reports, which then
    // guards every part.
    QString ifMatch;
    QString storageClass;                 // empty: the source's class
    qint64 partSize = Q_INT64_C(512) * 1024 * 1024;   // grown as needed for 10 000 parts
};

struct CopyResult {
    QString etag;
    QString versionId;
    QString sourceEtag;
    qint64 size = 0;
    int parts = 0;
};

// Server-side copy of an object of any size with UploadPartCopy (PLAN.md
// §6 M4: objects above CopyObject's 5 GiB): CreateMultipartUpload with the
// source's content type, metadata and storage class, the parts as byte
// ranges in parallel under the account's AdaptiveConcurrency — each with
// x-amz-copy-source-if-match — then CompleteMultipartUpload. A lost
// Complete answer is resolved with HEAD. The copy is checked for the
// source's size; a failure aborts the upload. ACLs and tags are not copied
// (as with CopyObject).
//
// Thread affinity as Uploader.
class QBUCKET_S3_TRANSFER_EXPORT Copier : public QObject {
    Q_OBJECT

public:
    Copier(Session* session, CopyRequest request, AdaptiveConcurrency* limiter, QObject* parent = nullptr);
    ~Copier() override;

    [[nodiscard]] const CopyRequest& request() const;
    [[nodiscard]] qint64 bytesDone() const;
    [[nodiscard]] qint64 bytesTotal() const;
    [[nodiscard]] const Result<CopyResult>& result() const;

    void start();
    // Stops and aborts the upload; the destination stays untouched.
    void cancel();

Q_SIGNALS:
    void progress(qint64 done, qint64 total);
    void finished();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qbucket::s3::transfer
