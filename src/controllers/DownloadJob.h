#pragma once

#include "controllers/Job.h"
#include "controllers/LocalPathMapper.h"
#include "controllers/PlanFile.h"
#include "controllers/TransferJobStore.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QElapsedTimer>
#include <QSet>

#include <atomic>
#include <deque>
#include <memory>

namespace qbucket {

namespace s3::transfer {
class Downloader;
}

// Downloads objects and folders into a local folder (PLAN.md §6 M3):
//   Plan    — selected objects are looked up afresh (HEAD), folders listed
//             completely; a worker then maps every key onto a safe local
//             path (LocalPathMapper: never outside the target, names the
//             system can't hold replaced), finds collisions ("a" and "a/b")
//             and what exists locally already.
//   Preview — count, bytes, the local files that exist (both sides' size
//             and date) and the policy for them, renamed / left-out names.
//   Execute — up to kParallelFiles objects at a time through
//             s3::transfer::Downloader: .qbpart, ranges in parallel, If-Match
//             on the ETag the plan saw, verified (MD5 / MD5-of-MD5s, else
//             size), renamed into place, mtime from the server. Pausable.
// Only reads from the server: allowed on read-only accounts.
class DownloadJob : public Job {
    Q_OBJECT

public:
    struct Item {
        QString key;          // object key, or folder prefix with "/"
        bool folder = false;
        QString localName;    // explicit path inside the target (Retry Failed); empty: derived
        QString versionId = {};   // an object's earlier version; empty: the current one
    };
    struct Options {
        // false: a single object without a local conflict on a
        // non-production account starts without the preview.
        bool alwaysConfirm = true;
    };

    static constexpr int kParallelFiles = 8;
    static constexpr int kParallelHeads = 8;
    static constexpr int kSamples = 10;
    static constexpr int kConflictSamples = 20;

    DownloadJob(const Account& account, const QString& bucket, const QList<Item>& items, const QString& targetDir,
                const QString& planDir, Options options, QObject* parent = nullptr);
    // Continues a job of an earlier session (TransferJobStore).
    DownloadJob(const Account& account, const TransferJobStore::Record& record, QObject* parent = nullptr);
    ~DownloadJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] bool isDestructive() const override { return false; }
    [[nodiscard]] bool isMutating() const override { return false; }
    [[nodiscard]] bool needsConfirmation() const override;
    [[nodiscard]] bool canPause() const override { return true; }
    [[nodiscard]] TransferKind transferKind() const override { return TransferKind::Download; }
    void suspend() override;
    [[nodiscard]] QList<ActiveItem> activeItems() const override;
    [[nodiscard]] bool isResumed() const { return m_resuming; }

    [[nodiscard]] QString targetDir() const { return m_target; }
    [[nodiscard]] QList<Item> failedItems() const { return m_failedItems; }
    [[nodiscard]] qint64 md5Verified() const { return m_md5Verified; }
    [[nodiscard]] qint64 sizeVerified() const { return m_sizeVerified; }

    // What the worker found (public for its test).
    struct LocalCheck {
        qint64 files = 0;
        qint64 folders = 0;
        qint64 bytes = 0;
        qint64 conflicts = 0;
        QStringList conflictSamples;
        qint64 renamed = 0;
        QStringList renamedSamples;
        qint64 skipped = 0;
        QStringList notes;
        QStringList samples;
        QString error;
    };
    // Raw records (key, relative name, size, ETag, mtime ms, storage class,
    // version id) → final records (key, local path, size, ETag, mtime ms,
    // local state, local size, local mtime ms, version id) for files that
    // can be written. Fields are only ever appended (plans are resumed).
    static LocalCheck checkLocal(const QString& rawPath, const QString& finalPath, const QString& targetDir,
                                 LocalPathMapper::Rules rules, const std::atomic<bool>& cancel);

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
        QString path;           // relative local path ("/"-separated; folders end with "/")
        qint64 size = 0;
        QString etag;
        qint64 mtimeMs = 0;
        QString localState;     // "", "file"
        qint64 localSize = 0;
        qint64 localMtimeMs = 0;
        QString versionId;      // empty: the current version
        qint64 ordinal = -1;
        static Record from(const QStringList& fields);
    };
    struct Active {
        Record record;
        QString path;           // where it goes (keep both: another name)
        std::unique_ptr<s3::transfer::Downloader> downloader;
        int keepBothTry = 0;
    };

    void resumePlan_();
    void planNext_();
    void planFolder_(const Item& item);
    void planDone_();
    void checked_(const LocalCheck& check);
    void pump_();
    void startItem_(const Record& r);
    void download_(const std::shared_ptr<Active>& item, bool overwrite);
    void itemDone_(Active* raw);
    void itemSkipped_(const QString& key, const QString& why);
    void itemFailed_(const Record& r, const QString& reason);
    void updateProgress_(bool force);
    void maybeExecuted_();
    void markDone_(const Record& r);
    void end_();
    [[nodiscard]] TransferJobStore* jobStore_() const;
    [[nodiscard]] QString transferId_(const Record& r) const;
    [[nodiscard]] QString absolute_(const QString& relative) const;
    [[nodiscard]] Report report_() const;

    QList<Item> m_items;
    QString m_target;
    QString m_planDir;
    Options m_options;
    std::unique_ptr<QObject> m_run;
    std::unique_ptr<PlanFile> m_raw;
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<s3::ListObjectsPager> m_pager;
    std::shared_ptr<std::atomic<bool>> m_checkCancel;
    qsizetype m_planIndex = 0;
    int m_headsInFlight = 0;
    qint64 m_missing = 0;
    LocalCheck m_check;

    std::deque<Record> m_queue;
    bool m_readingPlan = false;
    bool m_planExhausted = false;
    QList<std::shared_ptr<Active>> m_active;
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
    qint64 m_readOrdinal = 0;
    bool m_resuming = false;
    TransferJobStore::Record m_record;
    QSet<qint64> m_doneOrdinals;
    QSet<QString> m_storedTransfers;
    QElapsedTimer m_progressClock;
};

} // namespace qbucket
