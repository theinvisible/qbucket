#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/transfer/Export.h>
#include <qbucket/s3/transfer/PartPlanner.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

namespace qbucket::s3::transfer {

class AdaptiveConcurrency;

struct DownloadRequest {
    QString bucket;
    QString key;
    QString versionId;                    // empty: the current version
    QString targetPath;                   // the final file name
    // Replace an existing target (atomically, after verification).
    // Otherwise an existing target fails the download before anything is
    // fetched.
    bool overwrite = false;
    // The ETag the caller expects (e.g. from the listing it planned with):
    // the object changed since → PreconditionFailed.
    QString expectEtag;
    bool keepModificationTime = true;     // the file's mtime := Last-Modified
    PartPlannerOptions planner;           // ranges for large objects
    // With a ResumeStore: the record's id and the application's account id.
    QString transferId;
    QString account;
    QJsonObject extra;
};

struct FileDownloadResult {
    QString path;                         // where the file is now
    qint64 size = 0;
    QString etag;
    QString versionId;
    QDateTime lastModified;
    int parts = 1;
    // Md5Verified: the bytes on disk hash to the ETag (single part) or to
    // its MD5-of-MD5s (multipart, part layout from HEAD ?partNumber=1).
    // SizeVerified: an ETag we can't recompute (SSE-C / KMS, unknown or
    // irregular part layout) — reported as such, never as verified.
    Integrity integrity = Integrity::SizeVerified;
    bool resumed = false;
    bool restarted = false;               // the object changed or the partial file was gone
};

// Downloads one object into a local file (PLAN.md §6 M3, §12.5): into
// "<target>.qbpart" — one GET, or ranges in parallel under the account's
// AdaptiveConcurrency, every request with If-Match on the object's ETag —
// then verified, renamed to the target and given the object's Last-Modified.
// A failed verification is an error; the .qbpart stays for inspection.
//
// With a ResumeStore every finished range is recorded; fromStore()
// continues after a crash: the object must still have the same ETag (else
// it starts over), recorded ranges are re-hashed from disk before they are
// trusted.
//
// Thread affinity as Uploader: one thread with an event loop; the Session
// and the limiter outlive it.
class QBUCKET_S3_TRANSFER_EXPORT Downloader : public QObject {
    Q_OBJECT

public:
    enum class State : quint8 { Idle, Running, Pausing, Paused, Finished };

    Downloader(Session* session, DownloadRequest request, AdaptiveConcurrency* limiter,
               std::shared_ptr<ResumeStore> store, QObject* parent = nullptr);
    ~Downloader() override;

    [[nodiscard]] static std::unique_ptr<Downloader> fromStore(Session* session, std::shared_ptr<ResumeStore> store,
                                                               const QString& transferId, AdaptiveConcurrency* limiter,
                                                               QObject* parent = nullptr);

    // "<target>.qbpart"
    [[nodiscard]] static QString partialPath(const QString& targetPath);

    [[nodiscard]] const DownloadRequest& request() const;
    [[nodiscard]] State state() const;
    [[nodiscard]] qint64 bytesDone() const;
    [[nodiscard]] qint64 bytesTotal() const;
    [[nodiscard]] int partsDone() const;
    [[nodiscard]] int partCount() const;
    [[nodiscard]] const Result<FileDownloadResult>& result() const;

    void start();
    void pause();
    void resume();
    // Stops; the .qbpart and the record are removed unless keepForResume.
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
