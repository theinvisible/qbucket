#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <QSet>

#include <memory>

namespace qbucket {

class InPlaceCopy;
class VersionWalk;

// Restores earlier versions (PLAN.md §6 M5): each one is copied over its
// object's current version — on a versioned bucket the current one stays
// as an earlier version, nothing is lost. The same safe path as an edit
// (InPlaceCopy): a fresh snapshot of the version (HEAD, tags, ACL),
// copied with REPLACE of exactly its headers, its class, canned ACL and
// SSE-S3, guarded by its ETag; the tags written back where RGW drops them;
// the object read back and compared.
// Plan: a fresh listing of each key — a version that no longer exists, a
// delete marker or the current version itself is skipped with the reason.
// Versions a copy can't carry over (SSE-C, KMS, archive class, custom ACL,
// tags / public ACL where writing them isn't confirmed) are skipped too.
// Where the bucket doesn't keep versions the current one is overwritten:
// the preview says "permanent".
class RestoreVersionsJob : public Job {
    Q_OBJECT

public:
    struct VersionRef {
        QString key;
        QString versionId;
    };
    static constexpr int kParallel = 4;
    static constexpr int kSamples = 10;

    RestoreVersionsJob(const Account& account, const QString& bucket, const QList<VersionRef>& refs,
                       const QString& planDir, QObject* parent = nullptr);
    ~RestoreVersionsJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    void setRecheckDelayForTesting(int ms);

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    struct Entry {
        QString key;
        QString versionId;
        qint64 size = 0;
        QString etag;
    };
    void checkCapabilities_();
    void planNext_();
    bool planPage_(const QList<s3::VersionEntry>& entries, bool last);
    void planDone_();
    void pump_();
    void start_(const Entry& e);
    void done_(const Entry& e, const QString& failure);
    void skipped_(const QString& key, const QString& why);
    void settled_();
    [[nodiscard]] Report report_() const;

    QList<VersionRef> m_refs;
    QStringList m_keys;            // distinct, in order
    QSet<QString> m_wanted;        // "key\0versionId" not found yet
    QString m_planDir;
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;
    std::unique_ptr<VersionWalk> m_walk;
    InPlaceCopy* m_copy = nullptr;   // child

    qsizetype m_keyIndex = 0;
    bool m_large = false;
    qint64 m_bytes = 0;
    QStringList m_samples;
    bool m_readTags = false;
    bool m_readAcl = false;
    bool m_writeTags = false;
    bool m_writeAcl = false;

    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    qint64 m_succeeded = 0;
    qint64 m_skipped = 0;
    QStringList m_notes;
    QStringList m_failures;
    qint64 m_failed = 0;
};

} // namespace qbucket
