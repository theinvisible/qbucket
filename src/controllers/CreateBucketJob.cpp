#include "controllers/CreateBucketJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"

#include <qbucket/s3/BucketConfig.h>

#include <QTimer>

namespace qbucket {

CreateBucketJob::CreateBucketJob(const Account& account, const QString& name, const Options& options, QObject* parent)
    : Job(account, name, parent),
      m_options(options)
{
    if (m_options.objectLock)
        m_options.versioning = true;   // object lock comes with versioning
    if (m_options.region.isEmpty())
        m_options.region = account.region;
}

CreateBucketJob::~CreateBucketJob() = default;

QString CreateBucketJob::title() const
{
    return tr("Create bucket “%1”").arg(bucket());
}

QString CreateBucketJob::nameProblem(const QString& name)
{
    using P = s3::BucketNameProblem;
    switch (s3::bucketNameProblem(name)) {
    case P::None: return {};
    case P::Length: return tr("A bucket name has 3 to 63 characters.");
    case P::Characters: return tr("A bucket name has lower-case letters, digits, dots and hyphens only.");
    case P::StartOrEnd: return tr("A bucket name starts and ends with a letter or a digit.");
    case P::AdjacentDots: return tr("A bucket name has no two dots in a row.");
    case P::DotNextToHyphen: return tr("A bucket name has no dot next to a hyphen.");
    case P::IpAddress: return tr("A bucket name can't look like an IP address.");
    case P::ReservedPrefix: return tr("A bucket name can't start with xn--, sthree- or amzn-s3-demo-.");
    case P::ReservedSuffix: return tr("A bucket name can't end with -s3alias, --ol-s3, .mrap, --x-s3 or --table-s3.");
    }
    return {};
}

// --- plan ------------------------------------------------------------------------

void CreateBucketJob::plan()
{
    m_run = std::make_unique<QObject>();
    QString refusal = nameProblem(bucket());
    if (refusal.isEmpty() && m_options.objectLock) {
        if (controller_()->capabilities()->decide(account().id, QString(), s3::Feature::ObjectLock, false)
            == s3::Decision::Blocked)
            refusal = tr("This server has no object lock.");
    }
    if (refusal.isEmpty() && m_options.versioning && !m_options.objectLock) {
        const OperationController::Check gate =
            controller_()->check(account().id, QString(), {s3::Feature::BucketVersioningWrite}, true);
        if (!gate.allowed)
            refusal = gate.reason;
    }
    if (!refusal.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, refusal] { fail_(refusal); }, Qt::QueuedConnection);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Not connected.")); }, Qt::QueuedConnection);
        return;
    }
    // Taken already? Asked first, not found out by creating.
    session->headBucket(bucket(), m_run.get(), [this](const s3::Result<void>& r) {
        if (r) {
            fail_(tr("A bucket “%1” exists already.").arg(bucket()));
            return;
        }
        if (r.error().category == s3::ErrorCategory::NotPermitted) {
            fail_(tr("The name “%1” is taken by someone else.").arg(bucket()));
            return;
        }
        if (r.error().category != s3::ErrorCategory::NotFound) {
            fail_(tr("Could not check the name: %1").arg(ConnectionTester::describe(r.error())));
            return;
        }
        Preview p;
        p.action = tr("Create");
        p.scope = QStringLiteral("s3://%1").arg(bucket());
        p.objects = -1;
        if (!m_options.region.isEmpty())
            p.details.append(tr("Region: %1").arg(m_options.region));
        if (m_options.objectLock) {
            p.details.append(tr("With object lock (and versioning). Object lock can't be switched off again."));
        } else if (m_options.versioning) {
            p.details.append(tr("With versioning."));
        }
        if (bucket().contains(u'.'))
            p.warnings.append(tr("A name with dots can't be reached over TLS by its host name; QBucket uses "
                                 "path-style requests for it."));
        setPreview_(p);
    });
}

// --- execute -------------------------------------------------------------------------

void CreateBucketJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    s3::CreateBucketParams params;
    params.bucket = bucket();
    params.region = m_options.region;
    params.objectLockEnabled = m_options.objectLock;
    session->createBucket(params, m_run.get(), [this](const s3::Result<void>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::CreateBucket));
        rec.message = m_options.objectLock ? QStringLiteral("with object lock") : QString();
        if (r) {
            rec.httpStatus = 200;
            journal_({rec});
            created_();
            return;
        }
        const s3::Error& e = r.error();
        rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
        rec.httpStatus = e.httpStatus;
        rec.requestId = e.requestId;
        rec.message = e.toString();
        journal_({rec});
        if (e.outcomeUnknown) {
            // The answer was lost: did it happen? Look, never send it again.
            s3::Session* s = session_();
            if (!s) {
                fail_(tr("The answer was lost; whether the bucket exists is not known."));
                return;
            }
            s->headBucket(bucket(), m_run.get(), [this](const s3::Result<void>& head) {
                if (head)
                    created_();
                else
                    fail_(tr("The answer was lost and the bucket isn't there."));
            });
            return;
        }
        if (e.code == QLatin1String("BucketAlreadyOwnedByYou"))
            fail_(tr("A bucket “%1” exists already.").arg(bucket()));
        else if (e.code == QLatin1String("BucketAlreadyExists"))
            fail_(tr("The name “%1” is taken by someone else.").arg(bucket()));
        else
            fail_(tr("The server refused: %1").arg(ConnectionTester::describe(e)));
    });
}

void CreateBucketJob::created_()
{
    if (m_options.versioning && !m_options.objectLock)
        enableVersioning_();
    else
        executed_();
}

void CreateBucketJob::enableVersioning_()
{
    s3::Session* session = session_();
    if (!session) {
        executed_();
        return;
    }
    s3::PutBucketVersioningParams params;
    params.bucket = bucket();
    params.status = s3::VersioningStatus::Enabled;
    m_versioningSent = true;
    session->putBucketVersioning(params, m_run.get(), [this](const s3::Result<s3::SubresourceWriteResult>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::PutBucketVersioning));
        rec.message = QStringLiteral("versioning Enabled");
        if (r) {
            rec.httpStatus = 200;
            rec.requestId = r->requestId;
        } else {
            rec.result = Journal::Result::Failed;
            rec.httpStatus = r.error().httpStatus;
            rec.message += QStringLiteral(": ") + r.error().toString();
        }
        journal_({rec});
        executed_();   // the bucket is there; verify says what the versioning is
    });
}

// --- verify ------------------------------------------------------------------------------

void CreateBucketJob::verify()
{
    verifyBucket_(1);
}

// A new bucket may answer 403 / 404 for a while on some gateways (Hetzner).
void CreateBucketJob::verifyBucket_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        Report r;
        r.succeeded = 1;
        r.verification = tr("not checked (not connected)");
        finish_(r);
        return;
    }
    session->headBucket(bucket(), m_run.get(), [this, attempt](const s3::Result<void>& r) {
        if (!r && attempt < kVerifyTries) {
            QTimer::singleShot(m_delayMs, m_run.get(), [this, attempt] { verifyBucket_(attempt + 1); });
            return;
        }
        if (!r) {
            Report rep;
            rep.succeeded = 1;
            rep.verification = tr("the bucket doesn't answer yet: %1").arg(ConnectionTester::describe(r.error()));
            finish_(rep);
            return;
        }
        if (m_options.versioning)
            verifySettings_(1);
        else {
            Report rep;
            rep.succeeded = 1;
            rep.verified = true;
            rep.verification = tr("the bucket is there");
            finish_(rep);
        }
    });
}

void CreateBucketJob::verifySettings_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        finish_(Report{1, 0, 0, {}, tr("not checked (not connected)"), false, {}});
        return;
    }
    session->getBucketVersioning(bucket(), m_run.get(), [this, attempt](const s3::Result<s3::BucketVersioning>& v) {
        const bool versioned = v && v->status == s3::VersioningStatus::Enabled;
        if (!versioned && attempt < kVerifyTries) {
            QTimer::singleShot(m_delayMs, m_run.get(), [this, attempt] { verifySettings_(attempt + 1); });
            return;
        }
        const auto done = [this, versioned](bool locked, const QString& lockText) {
            Report rep;
            rep.succeeded = 1;
            rep.verified = versioned && (!m_options.objectLock || locked);
            QStringList parts{tr("the bucket is there")};
            parts.append(versioned ? tr("versioning is enabled") : tr("versioning is NOT enabled"));
            if (m_options.objectLock)
                parts.append(lockText);
            rep.verification = parts.join(QStringLiteral(", "));
            finish_(rep);
        };
        if (!m_options.objectLock) {
            done(false, {});
            return;
        }
        s3::Session* s = session_();
        if (!s) {
            done(false, tr("object lock not checked"));
            return;
        }
        s->getBucketConfig(bucket(), s3::BucketConfig::ObjectLock, m_run.get(),
                           [done](const s3::Result<s3::BucketConfigDocument>& doc) {
            const s3::Result<s3::ObjectLockConfiguration> c =
                doc && doc->configured ? s3::parseObjectLockConfiguration(doc->body)
                                       : s3::Result<s3::ObjectLockConfiguration>(s3::ObjectLockConfiguration{});
            const bool locked = c && c->enabled;
            done(locked, locked ? CreateBucketJob::tr("object lock is enabled")
                                : CreateBucketJob::tr("object lock is NOT enabled"));
        });
    });
}

} // namespace qbucket
