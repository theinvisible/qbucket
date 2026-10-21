#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <memory>

namespace qbucket {

class VersionPurger;
class VersionWalk;

// Deletes a bucket (PLAN.md §6 M6), optionally with everything in it:
// every version and delete marker (or every object where the server lists
// no versions) and every unfinished multipart upload, for good.
//
// Plan: a fresh walk of the whole bucket into a plan file, the unfinished
// uploads listed; a bucket that isn't empty is refused unless emptying was
// asked for. MFA delete refuses emptying. The preview is permanent and
// always typed with the bucket's name; on object lock it offers the
// governance bypass. Execute: the versions go (VersionPurger), the uploads
// are aborted; then the bucket is listed again — empty, then after
// ProviderProfile::bucketEmptinessSettleSecs() once more (Hetzner's proxy
// deletes a bucket that looked empty a moment ago, with the objects just
// written) — and only then DeleteBucket. BucketNotEmpty is retried after
// a pause, listing again first. Verify: the bucket doesn't answer any
// more; its capability results are dropped.
class DeleteBucketJob : public Job {
    Q_OBJECT

public:
    static constexpr int kSamples = 10;
    static constexpr int kMaxUploads = 100000;
    static constexpr int kRetries = 3;

    DeleteBucketJob(const Account& account, const QString& bucket, bool emptyFirst, const QString& planDir,
                    QObject* parent = nullptr);
    ~DeleteBucketJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] bool emptyFirst() const { return m_emptyFirst; }
    // Tests: pauses (between retries, before the second emptiness check).
    void setDelaysForTesting(int retryMs, int settleMs)
    {
        m_retryMs = retryMs;
        m_settleMs = settleMs;
    }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    struct Upload {
        QString key;
        QString uploadId;
    };

    void walk_();
    void listUploads_(const QString& keyMarker, const QString& uploadIdMarker);
    void planDone_();
    void abortUploads_();
    void abortNext_();
    // Is the bucket empty now? (versions or objects, and uploads)
    void checkEmpty_(const std::function<void(bool empty, const QString& left)>& done);
    void beforeDelete_(int attempt);
    void deleteBucket_(int attempt);
    void verifyGone_(int attempt);
    [[nodiscard]] Report report_() const;

    bool m_emptyFirst;
    QString m_planDir;
    bool m_currentOnly = false;   // no version listing on this server
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;
    std::unique_ptr<VersionWalk> m_walk;
    VersionPurger* m_purger = nullptr;
    QList<Upload> m_uploads;
    qsizetype m_abortIndex = 0;
    qint64 m_versions = 0;
    qint64 m_markers = 0;
    qint64 m_bytes = 0;
    QStringList m_samples;
    qint64 m_aborted = 0;
    QStringList m_failures;
    qint64 m_failed = 0;
    QString m_verification;
    bool m_verified = false;
    bool m_bucketDeleted = false;
    int m_retryMs = 3000;
    int m_settleMs = -1;          // -1: the profile's
};

} // namespace qbucket
