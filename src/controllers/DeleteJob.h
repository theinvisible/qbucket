#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QSet>

#include <memory>

namespace qbucket {

// Deletes objects and whole folders (PLAN.md §12.2/§12.3):
//   Plan    — selected objects are looked up afresh (HEAD), folders listed
//             completely (flat, every page) into a PlanFile; never what the
//             view showed. The bucket's versioning / object-lock state
//             decides what the preview says ("recoverable via versions" vs.
//             "permanent").
//   Execute — DeleteObjects batches (several in flight) when BatchDelete is
//             confirmed, single DeleteObject calls otherwise; per-key
//             results, failures never abort the rest; cancel stops after
//             the batches in flight. Journaled per request.
//   Verify  — folders are listed again, single objects looked up; what is
//             still there is checked again after a pause (eventually
//             consistent listings) before it is reported.
class DeleteJob : public Job {
    Q_OBJECT

public:
    struct Item {
        QString key;           // object key, or folder prefix with "/"
        bool folder = false;
    };

    static constexpr int kSamples = 10;
    static constexpr int kParallelBatches = 2;
    static constexpr int kParallelSingles = 4;
    static constexpr int kVerifyHeadLimit = 1000;   // single objects looked up after deleting
    // Pauses before checking again what a listing still shows.
    static constexpr int kRecheckDelaysMs[] = {3000, 10000};

    DeleteJob(const Account& account, const QString& bucket, const QList<Item>& items, const QString& planDir,
              QObject* parent = nullptr);
    ~DeleteJob() override;

    [[nodiscard]] QString title() const override;
    // Every key that could not be deleted — for "Retry Failed".
    [[nodiscard]] QStringList failedKeys() const { return m_failedKeys; }
    [[nodiscard]] bool usedBatches() const { return m_useBatches; }

    // Shorter verification pauses for tests.
    void setRecheckDelaysForTesting(int firstMs, int secondMs) { m_recheck = {firstMs, secondMs}; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    void planNext_();
    void planFolder_(const QString& prefix);
    void planDone_();
    void pump_();
    void sendBatch_(const QStringList& keys);
    void sendSingle_(const QString& key);
    void settled_();
    void verifyPass_(int pass);
    void verifyFolders_(qsizetype index, QStringList remaining, int pass);
    void verifyObjects_(qsizetype index, QStringList remaining, int pass);
    void verifyResult_(const QStringList& remaining, int pass);
    void addFailure_(const QString& key, const QString& reason);
    [[nodiscard]] Report report_() const;

    QList<Item> m_items;
    QString m_planDir;
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;   // context of every request of this job
    std::unique_ptr<s3::ListObjectsPager> m_pager;

    // Planning
    qsizetype m_planIndex = 0;
    qint64 m_bytes = 0;
    QStringList m_samples;
    QStringList m_folders;            // prefixes to verify
    QStringList m_objects;            // selected single keys
    QStringList m_existing;           // of those, the ones that existed: verified afterwards

    // Executing
    bool m_useBatches = false;
    int m_batchSize = 1;
    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    qint64 m_succeeded = 0;
    qint64 m_skipped = 0;
    QStringList m_failedKeys;
    QStringList m_failures;
    QSet<QString> m_failedSet;

    // Verifying
    std::pair<int, int> m_recheck{kRecheckDelaysMs[0], kRecheckDelaysMs[1]};
    QString m_verification;
    bool m_verified = false;
};

} // namespace qbucket
