#include "accounts/QtKeychainBackend.h"

#include "accounts/QtKeychain.h"

namespace qbucket {
namespace {

KeychainBackend::Status statusOf(const QKeychain::Job& job)
{
    switch (job.error()) {
    case QKeychain::NoError:
        return KeychainBackend::Status::Ok;
    case QKeychain::EntryNotFound:
        return KeychainBackend::Status::NotFound;
    case QKeychain::NoBackendAvailable:
    case QKeychain::NotImplemented:
        return KeychainBackend::Status::NoBackend;
    case QKeychain::AccessDenied:
    case QKeychain::AccessDeniedByUser:
        return KeychainBackend::Status::AccessDenied;
    default:
        return KeychainBackend::Status::Failed;
    }
}

template <typename JobT>
JobT* prepare(const QString& service, const QString& key)
{
    auto* job = new JobT(service);
    job->setAutoDelete(true);
    job->setInsecureFallback(false);
    job->setKey(key);
    return job;
}

// A failed job's status, settled by the service check: no keychain service
// at all is NoBackend. The jobs' handlers hold a copy of the check, never the
// backend — a job may finish after it is gone.
void settle(const QtKeychainBackend::ServiceCheck& check, KeychainBackend::Status status, const QString& error,
            std::function<void(KeychainBackend::Status, const QString&)> done)
{
    using Status = KeychainBackend::Status;
    if (status != Status::Failed || !check) {
        done(status, error);
        return;
    }
    check([error, done = std::move(done)](bool exists) {
        if (exists)
            done(Status::Failed, error);
        else
            done(Status::NoBackend, QStringLiteral("nothing on the session bus offers a Secret Service or KWallet"));
    });
}

} // namespace

QtKeychainBackend::QtKeychainBackend(ServiceCheck serviceCheck)
    : m_serviceCheck(std::move(serviceCheck))
{
}

void QtKeychainBackend::read(const QString& service, const QString& key, ReadCallback done)
{
    auto* job = prepare<QKeychain::ReadPasswordJob>(service, key);
    QObject::connect(job, &QKeychain::Job::finished, job, [check = m_serviceCheck, job, done = std::move(done)] {
        const Status s = statusOf(*job);
        if (s == Status::Ok) {
            done(s, job->textData(), {});
            return;
        }
        settle(check, s, job->errorString(), [done](Status settled, const QString& error) { done(settled, {}, error); });
    });
    job->start();
}

void QtKeychainBackend::write(const QString& service, const QString& key, const QString& secret,
                              DoneCallback done)
{
    auto* job = prepare<QKeychain::WritePasswordJob>(service, key);
    job->setTextData(secret);
    QObject::connect(job, &QKeychain::Job::finished, job, [check = m_serviceCheck, job, done = std::move(done)] {
        settle(check, statusOf(*job), job->errorString(), done);
    });
    job->start();
}

void QtKeychainBackend::remove(const QString& service, const QString& key, DoneCallback done)
{
    auto* job = prepare<QKeychain::DeletePasswordJob>(service, key);
    QObject::connect(job, &QKeychain::Job::finished, job, [check = m_serviceCheck, job, done = std::move(done)] {
        // Removing what isn't there is fine.
        const Status s = statusOf(*job);
        settle(check, s == Status::NotFound ? Status::Ok : s, job->errorString(), done);
    });
    job->start();
}

} // namespace qbucket
