#include "controllers/ConnectionTester.h"

#include <qbucket/s3/providers/ProviderRegistry.h>

namespace qbucket {

ConnectionTester::ConnectionTester(QObject* parent)
    : QObject(parent)
{
}

ConnectionTester::~ConnectionTester()
{
    cancel();
}

s3::SessionOptions ConnectionTester::sessionOptionsFor(const Account& account,
                                                       std::shared_ptr<s3::CredentialsProvider> credentials)
{
    s3::SessionOptions o;
    o.profile = s3::ProviderRegistry::builtin().findOrGeneric(account.providerId);
    o.endpoint.region = account.region.isEmpty() ? o.profile->signingRegion(QString()) : account.region;
    o.endpoint.url = account.endpoint.isEmpty() ? o.profile->endpointFor(o.endpoint.region)
                                                : QUrl(account.endpoint);
    o.endpoint.addressing = account.addressing;
    o.credentials = std::move(credentials);
    o.transferTimeoutMs = 30'000;
    return o;
}

QString ConnectionTester::describe(const s3::Error& e)
{
    using C = s3::ErrorCategory;
    switch (e.category) {
    case C::Authentication:
        return tr("The access key or the secret key is wrong.");
    case C::NotPermitted:
        return tr("Connected, but these credentials may not do this (%1).").arg(e.code);
    case C::ClockSkew:
        return tr("The server rejected the request time; check the system clock.");
    case C::Redirect:
        return tr("The bucket or account lives in another region than the one selected.");
    case C::Tls:
        return tr("Secure connection failed: %1").arg(e.message);
    case C::Transient:
        return tr("The server could not be reached: %1").arg(e.message);
    case C::NotFound:
        return tr("Not found: %1").arg(e.resource.isEmpty() ? e.message : e.resource);
    case C::Credentials:
        return tr("Credentials are not available: %1").arg(e.message);
    case C::InvalidRequest:
        return tr("The settings are not valid: %1").arg(e.message);
    case C::Cancelled:
        return tr("Cancelled.");
    default:
        return tr("Unexpected answer: %1").arg(e.toString());
    }
}

void ConnectionTester::start(const Account& account, const QString& secretAccessKey)
{
    cancel();
    s3::Credentials c;
    c.accessKeyId = account.accessKeyId.trimmed();
    c.secretAccessKey = secretAccessKey;
    s3::SessionOptions options =
        sessionOptionsFor(account, std::make_shared<s3::StaticCredentialsProvider>(c));
    if (const QString problem = options.endpoint.validationError(); !problem.isEmpty()) {
        // Still asynchronous, like every other outcome.
        QMetaObject::invokeMethod(this, [this, problem] {
            emit finished(false, tr("The endpoint is not valid: %1").arg(problem));
        }, Qt::QueuedConnection);
        return;
    }

    m_session = std::make_unique<s3::Session>(std::move(options));
    m_run = std::make_unique<QObject>();
    const QStringList external = account.externalBuckets;
    m_session->listBuckets(m_run.get(), [this, external](s3::Result<s3::BucketList> r) {
        if (r) {
            finish_(true, tr("Connected — %n bucket(s) visible.", nullptr, int(r->buckets.size())));
            return;
        }
        if (r.error().category == s3::ErrorCategory::NotPermitted && !external.isEmpty()) {
            tryExternalBucket_(external.first());
            return;
        }
        finish_(false, describe(r.error()));
    });
}

void ConnectionTester::tryExternalBucket_(const QString& bucket)
{
    m_session->headBucket(bucket, m_run.get(), [this, bucket](s3::Result<void> r) {
        if (r)
            finish_(true, tr("Connected — bucket “%1” is reachable (listing buckets is not permitted).").arg(bucket));
        else
            finish_(false, describe(r.error()));
    });
}

void ConnectionTester::finish_(bool ok, const QString& message)
{
    stopSession_();
    emit finished(ok, message);
}

void ConnectionTester::cancel()
{
    stopSession_();
}

void ConnectionTester::stopSession_()
{
    m_run.reset();   // pending callbacks of this run are dropped
    // Stop the session without waiting on its worker thread.
    if (s3::Session* s = m_session.release()) {
        connect(s, &s3::Session::shutdownFinished, s, &QObject::deleteLater);
        s->shutdown();
    }
}

} // namespace qbucket
