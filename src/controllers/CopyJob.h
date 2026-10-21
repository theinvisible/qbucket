#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QSet>

#include <memory>

namespace qbucket {

class ObjectCopyRunner;

// Copies or moves objects and folders server-side within one account, also
// into another bucket (PLAN.md §6 M4, §12.3: move = copy → verify → delete
// source):
//   Plan    — the sources are looked up afresh (folders listed flat into a
//             PlanFile with size, ETag and storage class); every target too:
//             a folder's target prefix is listed and merged with the sorted
//             sources (memory bounded), single objects by HEAD — so the
//             preview knows each conflict. A folder can't go into itself, a
//             move not onto its own place; a copy onto its own place makes
//             duplicates ("keep both").
//   Preview — count, bytes, conflicts with both sides, the policy (skip /
//             overwrite / keep both), what a move deletes and whether the
//             bucket keeps versions.
//   Execute — per object through ObjectCopyRunner (CopyObject or a
//             multipart copy, guarded by the planned ETag, HEAD of the copy,
//             a move deletes the source only then). Unless overwriting, the
//             target is looked up right before the copy (a small race window
//             remains: CopyObject has no reliable If-None-Match).
//   Verify  — every copy was checked on its own; a move's sources hold
//             nothing but what failed.
class CopyJob : public Job {
    Q_OBJECT

public:
    enum class Mode { Copy, Move };
    struct Item {
        QString key;           // an object key, or a folder prefix ending with "/"
        bool folder = false;
    };
    static constexpr int kParallel = 4;
    static constexpr int kSamples = 10;
    static constexpr int kConflictSamples = 20;
    static constexpr int kVerifyItems = 50;   // a move checks this many of its items' sources

    // Every item lies below `sourceBase` (the folder they were selected in,
    // "" for the bucket's root); `targetPrefix` ("" or ending with "/")
    // receives them with their path below sourceBase.
    CopyJob(const Account& account, const QString& sourceBucket, const QList<Item>& items, const QString& sourceBase,
            const QString& targetBucket, const QString& targetPrefix, Mode mode, const QString& planDir,
            QObject* parent = nullptr);
    ~CopyJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QStringList buckets() const override;
    // Between buckets: CrossBucketCopy; above 5 GiB: MultipartUpload.
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    // A move deletes; overwriting replaces.
    [[nodiscard]] bool isDestructive() const override;

    [[nodiscard]] Mode mode() const { return m_mode; }
    [[nodiscard]] QString targetBucket() const { return m_targetBucket; }
    [[nodiscard]] QString targetPrefix() const { return m_targetPrefix; }
    // Source keys that failed (a move left them where they were).
    [[nodiscard]] QStringList failedKeys() const { return m_failedKeys; }
    // For tests: the move check's second look at a source (Hetzner's
    // listings lag behind deletes for a moment).
    void setRecheckDelayForTesting(int ms) { m_recheckMs = ms; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    struct Record {
        QString key;
        qint64 size = 0;
        QString etag;
        QString storageClass;
        QString target;
        bool exists = false;
        qint64 targetSize = -1;
        qint64 targetMtimeMs = 0;
        qint64 mtimeMs = 0;
        [[nodiscard]] QStringList fields() const;
        static Record from(const QStringList& fields);
    };
    [[nodiscard]] QString targetFor_(const QString& key) const;
    [[nodiscard]] bool sameBucket_() const { return bucket() == m_targetBucket; }
    // plan
    void ensureFresh_(QStringList buckets);
    void nextItem_();
    void planObject_(const Item& item);
    void fetchSourcePage_();
    void mergeFolder_();
    void output_(const Record& r);
    void planDone_();
    // execute
    void pump_();
    void startRecord_(const Record& r);
    void findFreeName_(const Record& r, int n);
    void copy_(const Record& r, const QString& target);
    void settled_();
    void skipped_(const Record& r, const QString& why);
    void addFailure_(const QString& key, const QString& reason);
    // verify
    void verifyItem_(int index);
    [[nodiscard]] Report report_() const;

    QString m_targetBucket;
    QList<Item> m_items;
    QString m_sourceBase;
    QString m_targetPrefix;
    Mode m_mode;
    QString m_planDir;
    bool m_duplicate = false;   // a copy onto its own place
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;
    ObjectCopyRunner* m_runner = nullptr;   // child

    // planning
    int m_itemIndex = 0;
    Item m_current;
    std::unique_ptr<s3::ListObjectsPager> m_sourcePager;
    std::unique_ptr<s3::ListObjectsPager> m_targetPager;
    QList<s3::ObjectEntry> m_sourcePage;
    qsizetype m_sourceIndex = 0;
    QList<s3::ObjectEntry> m_targetPage;
    qsizetype m_targetIndex = 0;
    bool m_targetDone = false;
    bool m_large = false;
    qint64 m_bytes = 0;
    qint64 m_conflicts = 0;
    QStringList m_samples;
    QStringList m_conflictSamples;

    // execution
    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    qint64 m_succeeded = 0;
    qint64 m_skipped = 0;
    QStringList m_notes;
    QStringList m_failedKeys;
    QSet<QString> m_failedSet;
    QStringList m_failures;
    QString m_verification;
    bool m_verified = false;
    int m_recheckMs = 3000;
    bool m_rechecked = false;
    qint64 m_leftovers = 0;
};

} // namespace qbucket
