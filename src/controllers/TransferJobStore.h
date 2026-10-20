#pragma once

#include "controllers/Job.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>

#include <functional>

namespace qbucket {

// What an upload or download job needs to continue after the application
// quit or crashed (PLAN.md §6 M3): a record per job (<id>.json), its plan
// with the remaining items (<id>.plan, kept from the moment it executes)
// and the ordinals of the items already done (<id>.done, appended). The
// per-file part records live in the transfer engine's ResumeStore.
//
// File I/O runs on its own thread, in call order; done ordinals are
// buffered and written in batches. Callbacks come back to this object's
// thread. Thread affinity: GUI thread.
class TransferJobStore : public QObject {
    Q_OBJECT

public:
    struct Record {
        QString id;
        Job::TransferKind kind = Job::TransferKind::None;
        QUuid account;
        QString bucket;
        QString title;
        QString prefix;          // upload: where into
        QString target;          // download: the local folder
        Job::ConflictPolicy policy = Job::ConflictPolicy::Skip;
        qint64 items = 0;        // records in the plan
        qint64 bytes = 0;
        QDateTime started;
        bool suspended = false;  // stopped on purpose (else: the app died)
    };

    explicit TransferJobStore(const QString& directory, QObject* parent = nullptr);
    ~TransferJobStore() override;

    [[nodiscard]] QString directory() const { return m_dir; }
    [[nodiscard]] QString planPath(const QString& id) const;

    void save(const Record& record);
    void markDone(const QString& id, qint64 ordinal);
    // Record, plan and done list.
    void remove(const QString& id);

    void load(QObject* context, std::function<void(const QList<Record>&)> done);
    void loadDone(const QString& id, QObject* context, std::function<void(const QSet<qint64>&)> done);

    // Writes everything buffered and waits for the I/O thread (exit).
    bool waitForWrites(int timeoutMs);

private:
    void flushDone_();
    template <typename T>
    void post_(QObject* context, std::function<T()> work, std::function<void(const T&)> done);

    QString m_dir;
    QThreadPool m_io;
    QHash<QString, QByteArray> m_pendingDone;
    QTimer m_flush;
};

} // namespace qbucket
