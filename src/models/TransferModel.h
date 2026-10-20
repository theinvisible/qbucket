#pragma once

#include "controllers/Job.h"

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <deque>
#include <functional>

namespace qbucket {

class OperationController;

// The uploads and downloads of this session, for the transfer panel: one
// row per job — progress in bytes, speed (over the last seconds), time
// left, state — kept after the job ended until "clear finished", with the
// summary and a retry of what failed.
//
// Thread affinity: GUI thread. Updates are batched (every ~0.5 s).
class TransferModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { NameColumn, ProgressColumn, SpeedColumn, RemainingColumn, StateColumn, ColumnCount };
    enum Role {
        ProgressRole = Qt::UserRole + 1,   // 0…1000, -1 unknown
        FinishedRole,
        CanPauseRole,
        PausedRole,
        CanRetryRole,
        KindRole,                          // Job::TransferKind
    };

    explicit TransferModel(OperationController* operations, QObject* parent = nullptr);
    ~TransferModel() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    [[nodiscard]] Job* jobAt(int row) const;
    [[nodiscard]] int runningCount() const;
    void pause(int row);
    void resume(int row);
    void cancel(int row);
    // A new job for what failed in a finished row.
    void retry(int row);
    void clearFinished();

    // "1.2 MB/s", "3 min" — also for the status bar.
    [[nodiscard]] static QString formatSpeed(double bytesPerSecond);
    [[nodiscard]] static QString formatRemaining(qint64 seconds);

signals:
    void transferStarted();

private:
    struct Row {
        QPointer<Job> job;
        Job::TransferKind kind = Job::TransferKind::None;
        QString title;
        qint64 bytesDone = 0;
        qint64 bytesTotal = -1;
        qint64 itemsDone = 0;
        qint64 itemsTotal = 0;
        double speed = 0;                          // bytes per second
        std::deque<std::pair<qint64, qint64>> samples;   // (ms, bytes)
        bool finished = false;
        Job::Outcome outcome = Job::Outcome::None;
        QString summary;
        std::function<Job*()> retry;
        bool dirty = false;
    };

    void add_(Job* job);
    void finished_(Job* job);
    void tick_();
    [[nodiscard]] int rowOf_(const Job* job) const;
    [[nodiscard]] QString stateText_(const Row& r) const;

    OperationController* m_ops;
    QList<Row> m_rows;
    QTimer m_tick;
    QElapsedTimer m_clock;
};

} // namespace qbucket
