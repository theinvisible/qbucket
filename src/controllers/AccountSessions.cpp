#include "controllers/AccountSessions.h"

#include "accounts/AccountStore.h"
#include "accounts/KeychainCredentialsProvider.h"
#include "controllers/ConnectionTester.h"

namespace qbucket {

AccountSessions::AccountSessions(AccountStore* accounts, SecretStore* secrets, QObject* parent)
    : QObject(parent),
      m_accounts(accounts),
      m_secrets(secrets)
{
    connect(m_accounts, &AccountStore::accountsChanged, this, &AccountSessions::onAccountsChanged_);
}

AccountSessions::~AccountSessions()
{
    for (auto& [id, entry] : m_sessions)
        disconnect(entry.session.get(), nullptr, this, nullptr);
    m_sessions.clear();   // each ~Session cancels and joins its worker
    for (s3::Session* s : std::as_const(m_stopping)) {
        disconnect(s, nullptr, this, nullptr);
        delete s;
    }
}

s3::Result<s3::Session*> AccountSessions::session(const QUuid& account)
{
    if (m_shutDown)
        return s3::Error::make(s3::ErrorCategory::Cancelled, QStringLiteral("the application is shutting down"));
    if (const auto it = m_sessions.find(account); it != m_sessions.end())
        return it->second.session.get();

    const std::optional<Account> a = m_accounts->account(account);
    if (!a)
        return s3::Error::make(s3::ErrorCategory::InvalidRequest, QStringLiteral("unknown account"));

    s3::SessionOptions options =
        ConnectionTester::sessionOptionsFor(*a, KeychainCredentialsProvider::create(m_secrets, *a));
    // The tester's short timeout is for an interactive check; browsing uses
    // the library default.
    options.transferTimeoutMs = s3::SessionOptions{}.transferTimeoutMs;
    options.initialClockOffsetSecs = m_clockOffsets.value(account);
    if (const QString problem = options.endpoint.validationError(); !problem.isEmpty())
        return s3::Error::make(s3::ErrorCategory::InvalidRequest,
                               QStringLiteral("the endpoint is not valid: %1").arg(problem));

    auto session = std::make_unique<s3::Session>(std::move(options));
    s3::Session* s = session.get();
    connect(s, &s3::Session::clockSkewDetected, this, [this, account](qint64 offset) {
        m_clockOffsets.insert(account, offset);
        emit clockSkewDetected(account, offset);
    });
    connect(s, &s3::Session::capabilityLearned, this,
            [this, account](const QString& bucket, const QString& featureId) {
                emit capabilityLearned(account, bucket, featureId);
            });
    m_sessions.emplace(account, Entry{*a, std::move(session)});
    emit sessionStarted(account);
    return s;
}

void AccountSessions::reconnect(const QUuid& account)
{
    stop_(account);
}

void AccountSessions::shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;
    QList<QUuid> ids;
    for (const auto& [id, entry] : m_sessions)
        ids.append(id);
    for (const QUuid& id : std::as_const(ids))
        stop_(id);
    // Queued: callers connect to allStopped after calling shutdown().
    QMetaObject::invokeMethod(this, &AccountSessions::maybeAllStopped_, Qt::QueuedConnection);
}

void AccountSessions::onAccountsChanged_()
{
    QList<QUuid> outdated;
    for (const auto& [id, entry] : m_sessions) {
        const std::optional<Account> now = m_accounts->account(id);
        if (!now || !sameConnection(entry.account, *now))
            outdated.append(id);
    }
    for (const QUuid& id : std::as_const(outdated))
        stop_(id);
}

void AccountSessions::stop_(const QUuid& account)
{
    const auto it = m_sessions.find(account);
    if (it == m_sessions.end())
        return;
    s3::Session* s = it->second.session.release();
    m_sessions.erase(it);
    disconnect(s, nullptr, this, nullptr);
    m_stopping.insert(s);
    // The worker joins without blocking the GUI; deleted once it has.
    connect(s, &s3::Session::shutdownFinished, this, [this, s] {
        m_stopping.remove(s);
        s->deleteLater();
        maybeAllStopped_();
    });
    s->shutdown();
    emit sessionStopped(account);
}

void AccountSessions::maybeAllStopped_()
{
    // Once: the last shutdownFinished may come before the queued check.
    if (m_shutDown && m_stopping.isEmpty() && !m_allStoppedSent) {
        m_allStoppedSent = true;
        emit allStopped();
    }
}

} // namespace qbucket
