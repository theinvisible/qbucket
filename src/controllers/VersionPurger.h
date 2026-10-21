#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>

namespace qbucket {

class Job;
class PlanFile;

// Deletes the versions and delete markers of a plan file for a job — the
// executing part of DeleteVersionsJob and DeleteBucketJob: DeleteObjects
// batches with version ids where batch deletes are confirmed, single
// deletes otherwise, every result journaled on the job's behalf. Locked
// versions fail with the reason; the governance bypass is sent only when
// the job's user asked for it (Options::bypassGovernance).
//
// A plan record: key, versionId, "1" for a delete marker, "1" for the
// current entry, size, and a free field for the job (record()/targetOf()).
//
// Thread affinity: the job's (GUI) thread. Owned by the job.
class VersionPurger : public QObject {
    Q_OBJECT

public:
    struct Target {
        QString key;
        QString versionId;
        bool marker = false;
        bool latest = false;
        qint64 size = 0;
        QString extra;            // the job's own field (Undelete: the version revealed)
    };
    struct Options {
        bool useBatches = false;
        int batchSize = 1;
        bool bypassGovernance = false;
        QString markerMessage;    // journal text for a removed delete marker
    };

    static constexpr int kParallelBatches = 2;
    static constexpr int kParallelSingles = 4;

    [[nodiscard]] static QStringList record(const Target& t);
    [[nodiscard]] static Target targetOf(const QStringList& record);
    [[nodiscard]] static QString refId(const QString& key, const QString& versionId);

    VersionPurger(Job* job, PlanFile* plan, const Options& options);
    ~VersionPurger() override;

    // Reads the plan from the start; `done` once everything was sent and
    // answered (or after cancel(), with what was answered).
    void run(std::function<void()> done);
    void cancel();

    [[nodiscard]] bool usedBatches() const { return m_options.useBatches; }
    [[nodiscard]] qint64 succeeded() const { return m_succeeded; }
    [[nodiscard]] const QSet<QString>& failedSet() const { return m_failedSet; }   // refId()s
    [[nodiscard]] const QSet<QString>& failedKeys() const { return m_failedKeys; }
    [[nodiscard]] const QStringList& failures() const { return m_failures; }

signals:
    void progressed(qint64 handled);

private:
    void pump_();
    void sendBatch_(const QList<Target>& targets);
    void sendSingle_(const Target& t);
    void deleted_(const Target& t, const QString& requestId);
    void failed_(const Target& t, const QString& reason);
    void settled_();

    Job* m_job;
    PlanFile* m_plan;
    Options m_options;
    std::function<void()> m_done;
    std::unique_ptr<QObject> m_run;
    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    bool m_cancelled = false;
    bool m_finished = false;
    qint64 m_handled = 0;
    qint64 m_succeeded = 0;
    QSet<QString> m_failedSet;
    QSet<QString> m_failedKeys;
    QStringList m_failures;
};

} // namespace qbucket
