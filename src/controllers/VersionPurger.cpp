#include "controllers/VersionPurger.h"

#include "controllers/ConnectionTester.h"
#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <QHash>

namespace qbucket {

QStringList VersionPurger::record(const Target& t)
{
    return {t.key, t.versionId, t.marker ? QStringLiteral("1") : QStringLiteral("0"),
            t.latest ? QStringLiteral("1") : QStringLiteral("0"), QString::number(t.size), t.extra};
}

VersionPurger::Target VersionPurger::targetOf(const QStringList& record)
{
    Target t;
    t.key = record.value(0);
    t.versionId = record.value(1);
    t.marker = record.value(2) == QLatin1String("1");
    t.latest = record.value(3) == QLatin1String("1");
    t.size = record.value(4).toLongLong();
    t.extra = record.value(5);
    return t;
}

QString VersionPurger::refId(const QString& key, const QString& versionId)
{
    return key + QChar(u'\0') + versionId;
}

VersionPurger::VersionPurger(Job* job, PlanFile* plan, const Options& options)
    : QObject(job),
      m_job(job),
      m_plan(plan),
      m_options(options)
{
    m_options.batchSize = m_options.useBatches ? std::clamp(m_options.batchSize, 1, 1000) : 1;
}

VersionPurger::~VersionPurger() = default;

void VersionPurger::run(std::function<void()> done)
{
    m_done = std::move(done);
    m_run = std::make_unique<QObject>();
    m_plan->rewind();
    m_eof = false;
    pump_();
}

void VersionPurger::cancel()
{
    m_cancelled = true;
    settled_();
}

void VersionPurger::pump_()
{
    const int parallel = m_options.useBatches ? kParallelBatches : kParallelSingles;
    if (m_cancelled || m_reading || m_eof || m_inFlight >= parallel) {
        settled_();
        return;
    }
    m_reading = true;
    m_plan->readNextRecords(m_options.batchSize, [this](const QList<QStringList>& records) {
        m_reading = false;
        if (m_finished)
            return;
        if (records.isEmpty()) {
            m_eof = true;
            settled_();
            return;
        }
        if (m_cancelled) {
            settled_();
            return;
        }
        QList<Target> targets;
        targets.reserve(records.size());
        for (const QStringList& r : records)
            targets.append(targetOf(r));
        if (m_options.useBatches)
            sendBatch_(targets);
        else
            sendSingle_(targets.first());
        pump_();
    });
}

void VersionPurger::sendBatch_(const QList<Target>& targets)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        for (const Target& t : targets)
            failed_(t, tr("not connected"));
        return;
    }
    s3::DeleteObjectsParams params;
    params.bucket = m_job->bucket();
    params.bypassGovernanceRetention = m_options.bypassGovernance;
    params.objects.reserve(targets.size());
    for (const Target& t : targets)
        params.objects.append({t.key, t.versionId});
    ++m_inFlight;
    session->deleteObjects(params, m_run.get(), [this, targets](const s3::Result<s3::DeleteObjectsResult>& r) {
        --m_inFlight;
        Journal::Record batch = m_job->record_(s3::operationName(s3::Operation::DeleteObjects), targets.first().key);
        batch.bypassGovernance = m_options.bypassGovernance;
        if (r) {
            QHash<QString, const s3::DeleteObjectsResult::Failed*> refused;
            for (const auto& f : r->failed)
                refused.insert(refId(f.key, f.versionId), &f);
            for (const Target& t : targets) {
                if (const auto* f = refused.value(refId(t.key, t.versionId))) {
                    Journal::Record one = m_job->record_(s3::operationName(s3::Operation::DeleteObjects), t.key);
                    one.versionId = t.versionId;
                    one.result = Journal::Result::Failed;
                    one.httpStatus = 200;
                    one.requestId = r->requestId;
                    one.message = QStringLiteral("%1: %2").arg(f->code, f->message);
                    one.bypassGovernance = m_options.bypassGovernance;
                    m_job->journal_({one});
                    failed_(t, f->code == QLatin1String("AccessDenied")
                                   ? tr("protected by object lock, or not permitted (%1)").arg(f->message)
                                   : (f->code.isEmpty() ? f->message : f->code));
                } else {
                    deleted_(t, r->requestId);
                }
            }
            batch.httpStatus = 200;
            batch.requestId = r->requestId;
            batch.result = r->failed.isEmpty() ? Journal::Result::Ok : Journal::Result::Failed;
            batch.message = QStringLiteral("versions, batch of %1: %2 failed").arg(targets.size()).arg(r->failed.size());
        } else {
            const s3::Error& e = r.error();
            const QString reason = e.outcomeUnknown ? tr("the answer was lost — they may or may not be deleted")
                                                    : ConnectionTester::describe(e);
            for (const Target& t : targets)
                failed_(t, reason);
            batch.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            batch.httpStatus = e.httpStatus;
            batch.requestId = e.requestId;
            batch.message = QStringLiteral("versions, batch of %1: %2").arg(targets.size()).arg(e.toString());
        }
        m_job->journal_({batch});
        m_handled += targets.size();
        emit progressed(m_handled);
        pump_();
    });
}

void VersionPurger::sendSingle_(const Target& t)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        failed_(t, tr("not connected"));
        return;
    }
    s3::DeleteObjectParams params;
    params.bucket = m_job->bucket();
    params.key = t.key;
    params.versionId = t.versionId;
    params.bypassGovernanceRetention = m_options.bypassGovernance;
    ++m_inFlight;
    session->deleteObject(params, m_run.get(), [this, t](const s3::Result<s3::DeleteObjectResult>& r) {
        --m_inFlight;
        if (r) {
            deleted_(t, r->requestId);
        } else {
            const s3::Error& e = r.error();
            Journal::Record rec = m_job->record_(s3::operationName(s3::Operation::DeleteObject), t.key);
            rec.versionId = t.versionId;
            rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message = e.toString();
            rec.bypassGovernance = m_options.bypassGovernance;
            m_job->journal_({rec});
            failed_(t, e.category == s3::ErrorCategory::NotPermitted
                           ? tr("protected by object lock, or not permitted (%1)").arg(ConnectionTester::describe(e))
                           : ConnectionTester::describe(e));
        }
        ++m_handled;
        emit progressed(m_handled);
        pump_();
    });
}

void VersionPurger::deleted_(const Target& t, const QString& requestId)
{
    ++m_succeeded;
    Journal::Record rec = m_job->record_(
        s3::operationName(m_options.useBatches ? s3::Operation::DeleteObjects : s3::Operation::DeleteObject), t.key);
    rec.versionId = t.versionId;
    rec.httpStatus = m_options.useBatches ? 200 : 204;
    rec.requestId = requestId;
    rec.bypassGovernance = m_options.bypassGovernance;
    if (t.versionId.isEmpty())
        rec.message = QStringLiteral("object deleted");   // a server without versions
    else
        rec.message = t.marker ? m_options.markerMessage : QStringLiteral("version deleted permanently");
    m_job->journal_({rec});
}

void VersionPurger::failed_(const Target& t, const QString& reason)
{
    m_failedSet.insert(refId(t.key, t.versionId));
    m_failedKeys.insert(t.key);
    if (m_failures.size() < Job::kMaxListed)
        m_failures.append(t.versionId.isEmpty() ? QStringLiteral("%1: %2").arg(t.key, reason)
                                                : QStringLiteral("%1 (%2): %3").arg(t.key, t.versionId, reason));
}

void VersionPurger::settled_()
{
    if (m_finished || m_inFlight > 0 || m_reading)
        return;
    if (!m_cancelled && !m_eof)
        return;
    m_finished = true;
    if (m_done) {
        const std::function<void()> done = std::move(m_done);
        done();
    }
}

} // namespace qbucket
