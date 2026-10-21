#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <QDateTime>
#include <QSet>

#include <memory>

namespace qbucket {

class VersionWalk;

// Removes versions and delete markers (PLAN.md §12.3 "versioned buckets",
// §13.5 M5) — three kinds, one engine:
//   Versions   — exactly the selected versions / markers, for good;
//   Everything — every version and delete marker of objects / folders, for
//                good (the objects are gone, nothing left to restore);
//   Undelete   — the delete markers on top of each object's newest version
//                (optionally only those set after a moment): the objects
//                come back. A key with markers only is left alone.
// Plan: a fresh ListObjectVersions of every key / folder, never what the
// view showed; a selected version that no longer exists is skipped, "is a
// delete marker" comes from the listing only. Refused while MFA delete is
// on (QBucket sends no MFA codes). Execute: DeleteObjects batches with
// version ids where BatchDelete is confirmed, single deletes otherwise;
// locked versions fail with the reason, never with a bypass. Verify: the
// keys / folders listed again (Versions, Everything) or the objects looked
// up (Undelete), once more after a pause before a mismatch is reported.
class DeleteVersionsJob : public Job {
    Q_OBJECT

public:
    enum class Kind { Versions, Everything, Undelete };
    struct VersionRef {
        QString key;
        QString versionId;
    };
    struct Item {
        QString key;           // an object key, or a folder prefix ending with "/"
        bool folder = false;
    };

    static constexpr int kSamples = 10;
    static constexpr int kParallelBatches = 2;
    static constexpr int kParallelSingles = 4;
    static constexpr int kVerifyHeadLimit = 1000;
    static constexpr int kRecheckDelayMs = 3000;

    // Exactly these versions / markers, for good.
    static DeleteVersionsJob* versions(const Account& account, const QString& bucket, const QList<VersionRef>& refs,
                                       const QString& planDir);
    // Every version and marker of these objects and folders, for good.
    static DeleteVersionsJob* everything(const Account& account, const QString& bucket, const QList<Item>& items,
                                         const QString& planDir);
    // These objects / everything deleted in these folders back: the delete
    // markers over the newest version go (only those set at or after
    // `deletedAfter`, when valid).
    static DeleteVersionsJob* undelete(const Account& account, const QString& bucket, const QList<Item>& items,
                                       const QString& planDir, const QDateTime& deletedAfter = {});
    ~DeleteVersionsJob() override;

    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] bool isDestructive() const override { return m_kind != Kind::Undelete; }
    [[nodiscard]] bool usedBatches() const { return m_useBatches; }
    void setRecheckDelayForTesting(int ms) { m_recheckMs = ms; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    struct Target {
        QString key;
        QString versionId;
        bool marker = false;
        bool latest = false;
        qint64 size = 0;
        QString revealsVersion;   // Undelete: the version that becomes current
    };
    struct Walk {
        QString prefix;
        bool exact = false;
    };

    DeleteVersionsJob(Kind kind, const Account& account, const QString& bucket, const QString& planDir);
    void addItems_(const QList<Item>& items);

    // plan
    void checkVersioning_();
    void planNext_();
    bool planPage_(const QList<s3::VersionEntry>& entries, bool last);
    void undeleteEntry_(const s3::VersionEntry& e);
    void finishUndeleteKey_();
    void addTarget_(const Target& t);
    void planDone_();
    // execute
    void pump_();
    void sendBatch_(const QList<Target>& targets);
    void sendSingle_(const Target& t);
    void deleted_(const Target& t, const QString& requestId);
    void failed_(const Target& t, const QString& reason);
    void settled_();
    // verify
    void verifyPass_(bool recheck);
    void verifyNextWalk_(bool recheck);
    void verifyNextHead_(bool recheck);
    void verifyDone_(bool recheck);
    [[nodiscard]] Report report_() const;
    [[nodiscard]] static Target targetOf(const QStringList& record);

    Kind m_kind;
    QString m_planDir;
    QList<VersionRef> m_refs;
    QList<Item> m_items;
    QDateTime m_deletedAfter;
    QList<Walk> m_walks;
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;
    std::unique_ptr<VersionWalk> m_walk;

    // planning
    qsizetype m_walkIndex = 0;
    QSet<QString> m_wanted;           // Versions: "key\0versionId" still to be found
    QSet<QString> m_planned;          // Versions: what is planned (verify)
    QString m_curKey;                 // Undelete: the key being looked at
    QList<s3::VersionEntry> m_curMarkers;
    bool m_curDecided = false;
    QString m_lastKey;
    qint64 m_keys = 0;
    qint64 m_versionCount = 0;
    qint64 m_markerCount = 0;
    qint64 m_currentCount = 0;        // current versions among them
    qint64 m_restoredKeys = 0;        // Undelete: objects that come back
    qint64 m_bytes = 0;
    QStringList m_samples;
    QList<QPair<QString, QString>> m_revealed;   // Undelete: key → version that comes back (verify)

    // executing
    bool m_useBatches = false;
    int m_batchSize = 1;
    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    qint64 m_succeeded = 0;
    qint64 m_skipped = 0;
    QStringList m_notes;
    QStringList m_failures;
    QSet<QString> m_failedSet;        // "key\0versionId"
    QSet<QString> m_failedKeys;

    // verifying
    qsizetype m_verifyIndex = 0;
    qint64 m_remaining = 0;
    QStringList m_remainingSamples;
    QString m_verification;
    bool m_verified = false;
    int m_recheckMs = kRecheckDelayMs;
};

} // namespace qbucket
