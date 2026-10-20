#include "controllers/Job.h"

#include "controllers/AccountSessions.h"
#include "controllers/OperationController.h"

namespace qbucket {

Job::Job(const Account& account, const QString& bucket, QObject* parent)
    : QObject(parent),
      m_id(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)),
      m_account(account),
      m_bucket(bucket)
{
}

Job::~Job() = default;

void Job::confirm()
{
    if (m_phase != Phase::AwaitingConfirmation || m_cancelled || m_confirmed || !m_controller)
        return;
    m_confirmed = true;
    m_controller->confirm_(this);
}

void Job::cancel()
{
    if (m_phase == Phase::Finished || m_cancelled)
        return;
    m_cancelled = true;
    const Phase at = m_phase;
    onCancel();
    // Before execution nothing was changed: done at once. While executing
    // the job reports what it did once its requests have stopped.
    if (at != Phase::Executing && at != Phase::Verifying && m_phase != Phase::Finished)
        finish_(Report{});
}

void Job::setPreview_(const Preview& preview)
{
    if (m_phase != Phase::Planning || m_cancelled)
        return;
    m_preview = preview;
    if (m_controller)
        m_controller->previewed_(this);
    setPhase_(Phase::AwaitingConfirmation);
    emit previewReady();
}

void Job::setProgress_(qint64 done, qint64 total)
{
    m_done = done;
    m_total = total;
    emit progressChanged();
}

void Job::executed_()
{
    if (m_phase != Phase::Executing)
        return;
    setPhase_(Phase::Verifying);
    verify();
}

void Job::finish_(const Report& report)
{
    if (m_phase == Phase::Finished)
        return;
    m_report = report;
    if (m_cancelled)
        m_outcome = Outcome::Cancelled;
    else if (report.failed == 0)
        m_outcome = Outcome::Succeeded;
    else if (report.succeeded == 0)
        m_outcome = Outcome::Failed;
    else
        m_outcome = Outcome::PartlyFailed;
    setPhase_(Phase::Finished);
    if (m_controller)
        m_controller->finished_(this);
    emit finished();
}

void Job::fail_(const QString& error)
{
    if (m_phase == Phase::Finished)
        return;
    m_error = error;
    m_outcome = m_cancelled ? Outcome::Cancelled : Outcome::Failed;
    setPhase_(Phase::Finished);
    if (m_controller)
        m_controller->finished_(this);
    emit finished();
}

void Job::refuse_(const QString& reason)
{
    if (m_phase == Phase::Finished)
        return;
    m_error = reason;
    m_outcome = Outcome::Refused;
    setPhase_(Phase::Finished);
    if (m_controller)
        m_controller->finished_(this);
    emit finished();
}

void Job::journal_(const QList<Journal::Record>& records)
{
    if (m_controller)
        m_controller->journal()->append(records);
}

Journal::Record Job::record_(const QString& operation, const QString& key) const
{
    Journal::Record r;
    r.jobId = m_id;
    r.account = m_account.id;
    r.accountName = m_account.name;
    r.environment = m_account.environment;
    r.bucket = m_bucket;
    r.key = key;
    r.operation = operation;
    return r;
}

s3::Session* Job::session_() const
{
    if (!m_controller)
        return nullptr;
    const s3::Result<s3::Session*> s = m_controller->sessions()->session(m_account.id);
    return s ? *s : nullptr;
}

void Job::setPhase_(Phase phase)
{
    if (phase == m_phase)
        return;
    m_phase = phase;
    emit phaseChanged(phase);
}

} // namespace qbucket
