#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/transfer/Export.h>
#include <qbucket/s3/transfer/PartPlanner.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace qbucket::s3::transfer {

class AdaptiveConcurrency;

struct UploadRequest {
    QString bucket;
    QString key;
    QString localPath;
    QString contentType;                  // empty: application/octet-stream
    QMap<QString, QString> metadata;
    // Instead of contentType / metadata (which must then be empty): every
    // settable header, e.g. those of an object replaced in place.
    std::optional<ObjectHeaders> headers;
    QString storageClass;
    QString serverSideEncryption;         // e.g. AES256; empty: none
    QString cannedAcl;                    // x-amz-acl (Feature::ObjectAclWrite); empty: none
    // If-None-Match: * on the PUT / Complete. Only where the caller knows
    // Feature::ConditionalWrites is confirmed — otherwise it must check for
    // an existing key itself.
    bool ifNoneMatch = false;
    // If-Match: <ETag> on the PUT / Complete — replace the object only if it
    // is still that one. Only where Feature::ConditionalOverwrite is
    // confirmed; excludes ifNoneMatch.
    QString ifMatch;
    PartPlannerOptions planner;
    // With a ResumeStore: the record's id (isValidTransferId) and the
    // application's account id for it.
    QString transferId;
    QString account;
    QJsonObject extra;                    // stored with the record
};

struct UploadResult {
    QString etag;
    QString versionId;
    QString requestId;
    QString uploadId;                     // multipart only
    qint64 size = 0;
    int parts = 1;
    bool multipart = false;
    // Md5Verified: the server's ETag equals the MD5 (single part) or the
    // MD5-of-MD5s of the parts we sent. SizeVerified: an ETag of another
    // kind (e.g. SSE-C / KMS); every part was still checked with Content-MD5.
    Integrity integrity = Integrity::SizeVerified;
    bool resumed = false;                 // continued an earlier upload
    bool restarted = false;               // the earlier upload was gone; started over
};

// Uploads one local file (PLAN.md §6 M3, §12.5): a single PUT below the
// multipart threshold, otherwise CreateMultipartUpload → parts in parallel
// under the account's AdaptiveConcurrency → CompleteMultipartUpload. Every
// body is streamed from disk with its Content-MD5, hashed beforehand on a
// thread pool (a part ahead of the uploads). With a ResumeStore, every
// finished part is recorded, so a crash or a restart continues where it
// stopped (resume()); ListParts on the server decides what is really there.
//
// A lost Complete answer is resolved with HEAD, never by a blind retry. The
// source file is checked (size + mtime) before and after: a file that
// changed while it was uploaded fails the upload.
//
// Thread affinity: lives on one thread with an event loop (usually the GUI
// thread); every call and every signal happens there. The Session and the
// limiter must outlive it.
class QBUCKET_S3_TRANSFER_EXPORT Uploader : public QObject {
    Q_OBJECT

public:
    enum class State : quint8 { Idle, Running, Pausing, Paused, Finished };

    // limiter: shared per account (null: at most 4 parts at a time).
    // store: optional; needs UploadRequest::transferId.
    Uploader(Session* session, UploadRequest request, AdaptiveConcurrency* limiter,
             std::shared_ptr<ResumeStore> store, QObject* parent = nullptr);
    ~Uploader() override;

    // Continues the stored upload `transferId` (from store->ids()).
    [[nodiscard]] static std::unique_ptr<Uploader> fromStore(Session* session, std::shared_ptr<ResumeStore> store,
                                                             const QString& transferId,
                                                             AdaptiveConcurrency* limiter,
                                                             QObject* parent = nullptr);

    [[nodiscard]] const UploadRequest& request() const;
    [[nodiscard]] State state() const;
    [[nodiscard]] qint64 bytesDone() const;
    [[nodiscard]] qint64 bytesTotal() const;
    [[nodiscard]] int partsDone() const;
    [[nodiscard]] int partCount() const;
    // Valid once finished() was emitted.
    [[nodiscard]] const Result<UploadResult>& result() const;

    void start();
    // No new parts; the ones in flight finish and are recorded. paused()
    // follows once nothing is in flight.
    void pause();
    void resume();
    // Stops everything. A multipart upload is aborted on the server and the
    // record removed — unless keepForResume (e.g. on application exit).
    void cancel(bool keepForResume = false);

Q_SIGNALS:
    void progress(qint64 done, qint64 total);
    void paused();
    void finished();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qbucket::s3::transfer
