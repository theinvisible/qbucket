#pragma once

#include "controllers/Job.h"
#include "controllers/LocalScanner.h"
#include "controllers/PlanFile.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>

#include <atomic>
#include <deque>
#include <memory>

class QTimer;

namespace qbucket {

namespace s3::transfer {
class Uploader;
}

// Uploads local files and folders into a bucket (PLAN.md §6 M3, §12.2):
//   Plan    — the sources are walked on a worker (LocalScanner: sorted,
//             streamed into a plan file, symlinked folders not followed,
//             empty folders → "name/" markers); the targets are looked up
//             afresh — folders by listing their prefix and merging it with
//             the sorted local records, single files by HEAD — so the
//             preview knows every conflict.
//   Preview — count, bytes, the conflicts with both sides' size and date,
//             the policy for them (skip / overwrite / keep both).
//   Execute — up to kParallelFiles files at a time through
//             s3::transfer::Uploader (multipart, Content-MD5, resume
//             records), all drawing parts from the account's limiter. A key
//             that must not be overwritten is written with If-None-Match
//             where conditional writes are confirmed, else checked with
//             HEAD right before (a small race window remains). Pausable.
//   Verify  — every upload was verified by the Uploader (ETag = MD5 /
//             MD5-of-MD5s, else size); the report says which.
class UploadJob : public Job {
    Q_OBJECT

public:
    // Exactly this file to this key (Retry Failed).
    struct Item {
        QString localPath;
        QString key;
    };
    struct Options {
        // false: a single file without conflicts on a non-production
        // account starts without the preview (chosen in a file dialog).
        bool alwaysConfirm = true;
    };

    static constexpr int kParallelFiles = 8;
    static constexpr int kParallelHeads = 8;
    static constexpr int kSamples = 10;
    static constexpr int kConflictSamples = 20;

    // sources: local files and folders, uploaded into `prefix` (empty: the
    // bucket's root; otherwise ending with "/").
    UploadJob(const Account& account, const QString& bucket, const QString& prefix, const QStringList& sources,
              const QString& planDir, Options options, QObject* parent = nullptr);
    UploadJob(const Account& account, const QString& bucket, const QList<Item>& items, const QString& planDir,
              QObject* parent = nullptr);
    ~UploadJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override { return {}; }
    // Replacing existing objects is.
    [[nodiscard]] bool isDestructive() const override;
    [[nodiscard]] bool needsConfirmation() const override;
    [[nodiscard]] bool canPause() const override { return true; }
    [[nodiscard]] QList<ActiveItem> activeItems() const override;

    [[nodiscard]] QList<Item> failedItems() const { return m_failedItems; }
    [[nodiscard]] qint64 md5Verified() const { return m_md5Verified; }
    [[nodiscard]] qint64 sizeVerified() const { return m_sizeVerified; }
    [[nodiscard]] QString prefix() const { return m_prefix; }

    // "name (n).ext" — a free name for "keep both".
    [[nodiscard]] static QString keepBothName(const QString& key, int n);

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;
    void onPause() override;
    void onResume() override;

private:
    struct Record {
        QString key;
        QString path;
        qint64 size = 0;
        qint64 mtimeMs = 0;
        bool marker = false;
        bool exists = false;
        qint64 remoteSize = -1;
        qint64 remoteMtimeMs = 0;
        QString remoteEtag;

        [[nodiscard]] QStringList fields() const;
        static Record from(const QStringList& fields);
    };
    struct Active {
        Record record;
        QString key;                                   // where it goes (keep both: another name)
        std::unique_ptr<s3::transfer::Uploader> uploader;
        bool conditional = false;                      // sent with If-None-Match
        int keepBothTry = 0;
    };

    // plan
    void scanned_(const LocalScanner::Result& result);
    void mergeNext_();
    void mergeHead_(const Record& r);
    void output_(const Record& r);
    void planDone_();
    [[nodiscard]] QString groupOf_(const QString& key) const;

    // execute
    void pump_();
    void startItem_(const Record& r);
    void resolveTarget_(const std::shared_ptr<Active>& item);
    void upload_(const std::shared_ptr<Active>& item);
    void putMarker_(const Record& r);
    void itemDone_(Active* item);
    void itemSkipped_(const Record& r, const QString& why);
    void itemFailed_(const Record& r, const QString& key, const QString& reason);
    void updateProgress_(bool force);
    void maybeExecuted_();
    [[nodiscard]] Report report_() const;

    QString m_prefix;
    QStringList m_sources;
    QList<Item> m_explicit;
    bool m_explicitMode = false;
    Options m_options;
    QString m_planDir;
    std::unique_ptr<QObject> m_run;          // context of every request
    std::unique_ptr<PlanFile> m_raw;         // scanned records
    std::unique_ptr<PlanFile> m_plan;        // with the target state
    std::shared_ptr<std::atomic<bool>> m_scanCancel;
    std::shared_ptr<std::atomic<qint64>> m_scanFound;
    QTimer* m_scanProgress = nullptr;
    LocalScanner::Result m_scan;

    // merge state
    QList<QStringList> m_batch;
    qsizetype m_cursor = 0;
    bool m_batchesDone = false;
    int m_headsInFlight = 0;
    QString m_group;
    std::unique_ptr<s3::ListObjectsPager> m_pager;
    QList<s3::ObjectEntry> m_remote;
    qsizetype m_remoteIndex = 0;
    bool m_groupDone = true;
    bool m_fetching = false;

    // plan totals
    qint64 m_files = 0;
    qint64 m_markers = 0;
    qint64 m_bytes = 0;
    qint64 m_conflicts = 0;
    QStringList m_samples;
    QStringList m_conflictSamples;

    // execute state
    bool m_conditional = false;
    std::deque<Record> m_queue;
    bool m_readingPlan = false;
    bool m_planExhausted = false;
    QList<std::shared_ptr<Active>> m_active;
    int m_inFlightMisc = 0;                  // marker PUTs, HEADs
    qint64 m_doneBytes = 0;
    qint64 m_processed = 0;
    qint64 m_succeeded = 0;
    qint64 m_skipped = 0;
    qint64 m_failed = 0;
    qint64 m_md5Verified = 0;
    qint64 m_sizeVerified = 0;
    QStringList m_failures;
    QStringList m_notes;
    QList<Item> m_failedItems;
    qint64 m_nextTransfer = 0;
    QElapsedTimer m_progressClock;
};

} // namespace qbucket
