#include "controllers/OperationController.h"

#include "accounts/AccountStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Job.h"
#include "controllers/Journal.h"

namespace qbucket {

OperationController::OperationController(AccountStore* accounts, AccountSessions* sessions,
                                         CapabilityController* capabilities, Journal* journal, ActivityLog* log,
                                         QObject* parent)
    : QObject(parent),
      m_accounts(accounts),
      m_sessions(sessions),
      m_capabilities(capabilities),
      m_journal(journal),
      m_log(log)
{
}

OperationController::~OperationController()
{
    // Whatever still runs stops; nothing new is sent.
    for (const QPointer<Job>& job : std::as_const(m_jobs)) {
        if (job)
            job->cancel();
    }
}

OperationController::Check OperationController::check(const QUuid& account, const QString& bucket,
                                                      const QList<s3::Feature>& features, bool destructive) const
{
    const std::optional<Account> a = m_accounts->account(account);
    if (!a)
        return {false, tr("The account no longer exists.")};
    if (a->readOnly)
        return {false, tr("“%1” is a read-only account. Switch that off in the account settings to change "
                          "anything.").arg(a->name)};
    for (const s3::Feature f : features) {
        if (m_capabilities->decide(account, bucket, f, destructive) == s3::Decision::Blocked)
            return {false, CapabilityController::reason(f, m_capabilities->entry(account, bucket, f), destructive)};
    }
    return {};
}

void OperationController::start(Job* job)
{
    job->setParent(this);
    job->m_controller = this;
    m_jobs.append(job);
    connect(job, &Job::finished, job, &QObject::deleteLater, Qt::QueuedConnection);
    emit jobStarted(job);
    QMetaObject::invokeMethod(this, [this, guard = QPointer<Job>(job)] {
        if (!guard || guard->phase() != Job::Phase::Created)
            return;
        const Check gate = check(guard->account().id, guard->bucket(), guard->requiredFeatures(),
                                 guard->isDestructive());
        if (!gate.allowed) {
            guard->refuse_(gate.reason);
            return;
        }
        guard->setPhase_(Job::Phase::Planning);
        guard->plan();
    }, Qt::QueuedConnection);
}

QList<Job*> OperationController::jobs() const
{
    QList<Job*> out;
    for (const QPointer<Job>& j : m_jobs) {
        if (j && j->phase() != Job::Phase::Finished)
            out.append(j);
    }
    return out;
}

void OperationController::previewed_(Job* job)
{
    if (!job->needsConfirmation()) {
        QMetaObject::invokeMethod(job, &Job::confirm, Qt::QueuedConnection);
        return;
    }
    Job::Preview& p = job->m_preview;
    if (job->isDestructive() && p.objects > kTypeToConfirmAbove && p.typeToConfirm.isEmpty()) {
        // Type the folder's (or bucket's) own name: hard to do by accident.
        QString name = p.scope;
        while (name.endsWith(u'/'))
            name.chop(1);
        p.typeToConfirm = name.mid(name.lastIndexOf(u'/') + 1);
    }
    emit previewNeeded(job);
}

void OperationController::confirm_(Job* job)
{
    const Check gate = check(job->account().id, job->bucket(), job->requiredFeatures(), job->isDestructive());
    if (!gate.allowed) {
        job->refuse_(gate.reason);
        return;
    }
    if (!job->isDestructive()) {
        execute_(job);
        return;
    }
    // Destructive: decide on results young enough to trust — probe if not.
    // The job stays "awaiting confirmation" meanwhile: cancelling it then
    // ends it at once, nothing having been sent.
    const QPointer<Job> guard(job);
    m_capabilities->ensureFresh(job->account().id, job->bucket(), kFreshBeforeDestructiveSecs, job,
                                [this, guard](bool fresh) {
        if (!guard || guard->isCancelled())
            return;
        if (!fresh) {
            guard->refuse_(tr("Could not confirm what the server supports right now; nothing was changed."));
            return;
        }
        const Check again = check(guard->account().id, guard->bucket(), guard->requiredFeatures(), true);
        if (!again.allowed) {
            guard->refuse_(again.reason);
            return;
        }
        execute_(guard);
    });
}

void OperationController::execute_(Job* job)
{
    job->setPhase_(Job::Phase::Executing);
    job->execute();
}

void OperationController::finished_(Job* job)
{
    const bool bad = job->outcome() == Job::Outcome::Failed || job->outcome() == Job::Outcome::PartlyFailed;
    const bool refused = job->outcome() == Job::Outcome::Refused;
    m_log->add(bad ? ActivityLog::Level::Error : (refused ? ActivityLog::Level::Warning : ActivityLog::Level::Info),
               summary(*job));
    emit jobFinished(job);
}

QString OperationController::summary(const Job& job)
{
    const Job::Report& r = job.report();
    QString what;
    switch (job.outcome()) {
    case Job::Outcome::None:
        break;
    case Job::Outcome::Succeeded:
        what = r.skipped > 0 ? tr("done: %1 changed, %2 skipped").arg(r.succeeded).arg(r.skipped)
                             : tr("done: %n changed", nullptr, int(r.succeeded));
        break;
    case Job::Outcome::PartlyFailed:
        what = tr("partly failed: %1 changed, %2 failed").arg(r.succeeded).arg(r.failed);
        break;
    case Job::Outcome::Failed:
        what = job.errorText().isEmpty() ? tr("failed: %n not changed", nullptr, int(r.failed))
                                         : tr("failed: %1").arg(job.errorText());
        break;
    case Job::Outcome::Cancelled:
        what = tr("cancelled: %n changed before", nullptr, int(r.succeeded));
        break;
    case Job::Outcome::Refused:
        what = tr("not started: %1").arg(job.errorText());
        break;
    }
    if (!r.verification.isEmpty())
        what += QStringLiteral(" — ") + r.verification;
    return tr("%1 — %2").arg(job.title(), what);
}

} // namespace qbucket
