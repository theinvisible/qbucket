#pragma once

#include <qbucket/s3/Capabilities.h>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QUuid>

namespace qbucket {

class AccountSessions;
class AccountStore;
class ActivityLog;
class CapabilityController;
class Job;
class Journal;
class TransferService;

// The one gate for every mutation (PLAN.md §12.1). Nothing in the app
// calls the library's writing API except a Job started here (a CI check
// enforces it). The controller
//   - refuses jobs of read-only accounts and jobs needing a feature the
//     capability safeguard blocks (Unknown never unlocks a destructive one),
//   - lets the job plan, adds the confirmation policy to its preview
//     (typed confirmation for large scopes) and asks the UI (previewNeeded),
//   - before executing a confirmed destructive job re-checks the gate on
//     capability results younger than kFreshBeforeDestructiveSecs,
//   - journals and reports every outcome to the Log.
//
// Thread affinity: GUI thread.
class OperationController : public QObject {
    Q_OBJECT

public:
    static constexpr int kFreshBeforeDestructiveSecs = 60;
    // A destructive job touching more objects has to be confirmed by typing.
    static constexpr qint64 kTypeToConfirmAbove = 1000;

    OperationController(AccountStore* accounts, AccountSessions* sessions, CapabilityController* capabilities,
                        Journal* journal, ActivityLog* log, QObject* parent = nullptr);
    ~OperationController() override;

    struct Check {
        bool allowed = true;
        QString reason;   // translated; why not
    };
    // May this account do something needing `features` in `bucket`?
    // Cheap and synchronous — for enabling actions and their tooltips.
    // mutating false (a download): allowed on read-only accounts.
    [[nodiscard]] Check check(const QUuid& account, const QString& bucket, const QList<s3::Feature>& features,
                              bool destructive, bool mutating = true) const;

    // Takes ownership and starts planning — or refuses (finished with
    // Outcome::Refused). Always asynchronous: connect to the job first.
    void start(Job* job);
    [[nodiscard]] QList<Job*> jobs() const;

    [[nodiscard]] AccountSessions* sessions() const { return m_sessions; }
    [[nodiscard]] AccountStore* accounts() const { return m_accounts; }
    [[nodiscard]] Journal* journal() const { return m_journal; }
    [[nodiscard]] CapabilityController* capabilities() const { return m_capabilities; }
    // Shared by the transfer jobs (limiters, resume store); set by the window.
    [[nodiscard]] TransferService* transfers() const { return m_transfers; }
    void setTransfers(TransferService* transfers) { m_transfers = transfers; }
    // Where bulk jobs keep their plan files (AppDataLocation/plans).
    [[nodiscard]] QString planDir() const { return m_planDir; }
    void setPlanDir(const QString& dir) { m_planDir = dir; }

    // The Log line for a finished job.
    [[nodiscard]] static QString summary(const Job& job);

signals:
    void jobStarted(qbucket::Job* job);
    // The job has planned; the UI shows the preview and calls confirm() or
    // cancel() on it.
    void previewNeeded(qbucket::Job* job);
    // Before the job is deleted (later, from the event loop).
    void jobFinished(qbucket::Job* job);

private:
    friend class Job;
    void previewed_(Job* job);
    void confirm_(Job* job);
    void execute_(Job* job);
    // check() for every bucket of the job.
    [[nodiscard]] Check checkJob_(const Job* job, bool destructive) const;
    void ensureFreshThenExecute_(Job* job, QStringList buckets);
    void finished_(Job* job);

    AccountStore* m_accounts;
    AccountSessions* m_sessions;
    CapabilityController* m_capabilities;
    Journal* m_journal;
    ActivityLog* m_log;
    QList<QPointer<Job>> m_jobs;
    QString m_planDir;
    TransferService* m_transfers = nullptr;
};

} // namespace qbucket
