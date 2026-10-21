#include "controllers/SetBucketVersioningJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"

#include <QTimer>

namespace qbucket {

namespace {

// For the journal (English, like every journal line).
QString statusName(s3::VersioningStatus status)
{
    switch (status) {
    case s3::VersioningStatus::Enabled: return QStringLiteral("Enabled");
    case s3::VersioningStatus::Suspended: return QStringLiteral("Suspended");
    case s3::VersioningStatus::Never: break;
    }
    return QStringLiteral("never enabled");
}

// For the user.
QString statusText(s3::VersioningStatus status)
{
    switch (status) {
    case s3::VersioningStatus::Enabled: return SetBucketVersioningJob::tr("enabled");
    case s3::VersioningStatus::Suspended: return SetBucketVersioningJob::tr("suspended");
    case s3::VersioningStatus::Never: break;
    }
    return SetBucketVersioningJob::tr("never enabled");
}

} // namespace

SetBucketVersioningJob::SetBucketVersioningJob(const Account& account, const QString& bucket,
                                               s3::VersioningStatus status, QObject* parent)
    : Job(account, bucket, parent),
      m_status(status)
{
}

SetBucketVersioningJob::~SetBucketVersioningJob() = default;

QString SetBucketVersioningJob::title() const
{
    return m_status == s3::VersioningStatus::Suspended ? tr("Suspend versioning of “%1”").arg(bucket())
                                                      : tr("Enable versioning of “%1”").arg(bucket());
}

QList<s3::Feature> SetBucketVersioningJob::requiredFeatures() const
{
    return {s3::Feature::BucketVersioningWrite};
}

void SetBucketVersioningJob::plan()
{
    m_run = std::make_unique<QObject>();
    if (m_status == s3::VersioningStatus::Never) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Versioning can only be enabled or suspended.")); },
                                  Qt::QueuedConnection);
        return;
    }
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 60, m_run.get(), [this](bool fresh) {
        if (!fresh) {
            fail_(tr("The bucket's current state could not be checked. Nothing was changed."));
            return;
        }
        const CapabilityController::Versioning v = controller_()->capabilities()->versioningOf(account().id, bucket());
        if (v.mfaDelete.value_or(false)) {
            fail_(tr("MFA delete is enabled on this bucket: changing its versioning needs a code from the MFA device, "
                     "which QBucket can't send. Nothing was changed."));
            return;
        }
        if (m_status == s3::VersioningStatus::Suspended && v.objectLock.value_or(false)) {
            fail_(tr("The bucket has object lock: its versioning can't be suspended. Nothing was changed."));
            return;
        }
        if (v.known && v.status == m_status) {
            Report r;
            r.verified = true;
            r.verification = tr("versioning is %1 already").arg(statusText(m_status));
            finish_(r);
            return;
        }
        Preview p;
        p.objects = -1;
        p.scope = QStringLiteral("s3://%1").arg(bucket());
        p.details.append(tr("Versioning now: %1.").arg(v.known ? statusText(v.status) : tr("unknown")));
        if (m_status == s3::VersioningStatus::Enabled) {
            p.action = tr("Enable Versioning");
            p.details.append(tr("From now on every overwrite and delete keeps the earlier version; deleting "
                                "creates a delete marker."));
            p.details.append(tr("Once enabled, versioning can only be suspended, never switched off; earlier "
                                "versions take storage until they are deleted."));
        } else {
            p.action = tr("Suspend Versioning");
            p.warnings.append(tr("From now on overwrites and deletes are permanent: what is written replaces the "
                                 "current version, a delete removes it. Versions kept so far stay."));
        }
        setPreview_(p);
    });
}

void SetBucketVersioningJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    s3::PutBucketVersioningParams params;
    params.bucket = bucket();
    params.status = m_status;
    session->putBucketVersioning(params, m_run.get(), [this](const s3::Result<s3::SubresourceWriteResult>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::PutBucketVersioning));
        rec.message = QStringLiteral("versioning %1").arg(statusName(m_status));
        if (!r) {
            const s3::Error& e = r.error();
            rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message += QStringLiteral(": ") + e.toString();
            journal_({rec});
            fail_(tr("The server refused: %1").arg(ConnectionTester::describe(e)));
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        journal_({rec});
        executed_();
    });
}

void SetBucketVersioningJob::verify()
{
    readBack_(1);
}

// Gateways may answer with the old setting for a while: a few tries before
// the mismatch is reported — and then it is reported, never assumed away.
void SetBucketVersioningJob::readBack_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        Report r;
        r.succeeded = 1;
        r.verification = tr("not checked (not connected)");
        finish_(r);
        return;
    }
    session->getBucketVersioning(bucket(), m_run.get(), [this, attempt](const s3::Result<s3::BucketVersioning>& v) {
        if ((!v || v->status != m_status) && attempt < kReadBackTries) {
            QTimer::singleShot(m_delayMs, m_run.get(), [this, attempt] { readBack_(attempt + 1); });
            return;
        }
        controller_()->capabilities()->recheck(account().id, bucket());
        Report r;
        r.succeeded = 1;
        if (v && v->status == m_status) {
            r.verified = true;
            r.verification = tr("read back: versioning is %1").arg(statusText(m_status));
        } else {
            r.verification = v ? tr("the server still reports versioning as %1 — check again in a few minutes")
                                     .arg(statusText(v->status))
                               : tr("not read back: %1").arg(ConnectionTester::describe(v.error()));
        }
        finish_(r);
    });
}

} // namespace qbucket
