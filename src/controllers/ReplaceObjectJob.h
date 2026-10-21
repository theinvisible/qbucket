#pragma once

#include "controllers/Job.h"
#include "controllers/ObjectSnapshot.h"

#include <memory>
#include <optional>

namespace qbucket {

namespace s3::transfer {
class Uploader;
}

// Replaces an object's content with a local file and keeps everything else
// (PLAN.md §12.3) — what "Open" uploads after the file was changed in
// another application:
//   Plan    — a fresh ObjectSnapshot (HEAD, tags, ACL). Changed or deleted
//             on the server since it was opened (the ETag then) is a
//             conflict: skipped unless the preview says overwrite / keep
//             both. An object a rewrite would lose state of (SSE-C / KMS,
//             retention, a legal hold, grants, unwritable tags) is refused.
//   Preview — what is kept; on an unversioned bucket the old content is
//             gone for good.
//   Execute — uploaded (Uploader) with every header of the snapshot, its
//             class, SSE-S3 and canned ACL; If-Match on the snapshot's ETag
//             where ConditionalOverwrite is confirmed, else HEAD right
//             before (a small window remains). Tags written back with
//             PutObjectTagging (RGW and Hetzner ignore x-amz-tagging there).
//   Verify  — read back: content (the Uploader's MD5 check), size, every
//             header, class, tags, ACL; once more after a moment.
class ReplaceObjectJob : public Job {
    Q_OBJECT

public:
    struct Request {
        QString key;
        QString localPath;
        QString openedEtag;       // the object's ETag when the file was opened
    };

    ReplaceObjectJob(const Account& account, const QString& bucket, const Request& request, QObject* parent = nullptr);
    ~ReplaceObjectJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] bool needsConfirmation() const override;
    [[nodiscard]] TransferKind transferKind() const override { return TransferKind::Upload; }

    [[nodiscard]] const Request& request() const { return m_request; }
    // After success: where the content went and its new ETag.
    [[nodiscard]] QString writtenKey() const { return m_targetKey; }
    [[nodiscard]] QString writtenEtag() const { return m_newEtag; }
    // For tests: how long verify waits before reading again.
    void setRecheckDelay(int ms) { m_recheckMs = ms; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    enum class Conflict { None, Changed, Deleted };
    void planned_(const s3::Result<ObjectSnapshot>& snapshot);
    void guardAndUpload_();
    void findFreeKey_(int attempt);
    void upload_(bool ifMatch, bool ifNoneMatch);
    void uploaded_();
    void writeTags_();
    void check_();
    void fail_report_(const QString& why);

    Request m_request;
    std::unique_ptr<QObject> m_run;          // context of this run's callbacks
    std::optional<ObjectSnapshot> m_snapshot;
    Conflict m_conflict = Conflict::None;
    qint64 m_localSize = -1;
    bool m_versioned = false;
    bool m_writeTags = false;
    bool m_writeAcl = false;
    QString m_canned;                        // a non-private canned ACL carried over
    QString m_targetKey;
    QString m_newEtag;
    std::unique_ptr<s3::transfer::Uploader> m_uploader;
    bool m_rechecked = false;
    int m_recheckMs = 3000;
    Report m_result;
};

} // namespace qbucket
