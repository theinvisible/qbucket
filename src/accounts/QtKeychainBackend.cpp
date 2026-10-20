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

} // namespace

void QtKeychainBackend::read(const QString& service, const QString& key, ReadCallback done)
{
    auto* job = prepare<QKeychain::ReadPasswordJob>(service, key);
    QObject::connect(job, &QKeychain::Job::finished, job, [job, done = std::move(done)] {
        const Status s = statusOf(*job);
        done(s, s == Status::Ok ? job->textData() : QString(), job->errorString());
    });
    job->start();
}

void QtKeychainBackend::write(const QString& service, const QString& key, const QString& secret,
                              DoneCallback done)
{
    auto* job = prepare<QKeychain::WritePasswordJob>(service, key);
    job->setTextData(secret);
    QObject::connect(job, &QKeychain::Job::finished, job, [job, done = std::move(done)] {
        done(statusOf(*job), job->errorString());
    });
    job->start();
}

void QtKeychainBackend::remove(const QString& service, const QString& key, DoneCallback done)
{
    auto* job = prepare<QKeychain::DeletePasswordJob>(service, key);
    QObject::connect(job, &QKeychain::Job::finished, job, [job, done = std::move(done)] {
        // Removing what isn't there is fine.
        const Status s = statusOf(*job);
        done(s == Status::NotFound ? Status::Ok : s, job->errorString());
    });
    job->start();
}

} // namespace qbucket
