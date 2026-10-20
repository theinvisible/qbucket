#pragma once

#include "domain/Account.h"

#include <qbucket/s3/Result.h>
#include <qbucket/s3/Session.h>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QUuid>

#include <map>
#include <memory>

namespace qbucket {

class AccountStore;
class SecretStore;

// One s3::Session per account — the account's browse thread (PLAN.md §3
// threading model). Created on first use, with the keychain secret through
// KeychainCredentialsProvider. A session whose connection settings changed
// or whose account was removed is stopped without blocking; the next
// session() call builds a fresh one. The clock offset a session reported is
// handed to its successor.
//
// Thread affinity: GUI thread. Destroy before the SecretStore.
class AccountSessions : public QObject {
    Q_OBJECT

public:
    AccountSessions(AccountStore* accounts, SecretStore* secrets, QObject* parent = nullptr);
    // Sessions still running are destroyed, each waiting at most ~2 s.
    // Prefer shutdown() + allStopped.
    ~AccountSessions() override;

    // The account's session, created if needed. Owned by AccountSessions;
    // don't keep the pointer past sessionStopped. Fails for an unknown
    // account, invalid endpoint settings or after shutdown().
    [[nodiscard]] s3::Result<s3::Session*> session(const QUuid& account);
    [[nodiscard]] bool hasSession(const QUuid& account) const { return m_sessions.count(account) != 0; }

    // Stops the account's session (e.g. after its secret changed); the next
    // session() call connects again.
    void reconnect(const QUuid& account);

    // Orderly stop of every session; allStopped follows once all worker
    // threads have ended (queued, also when nothing was running).
    void shutdown();
    [[nodiscard]] bool isShutDown() const { return m_shutDown; }
    [[nodiscard]] bool isStopping() const { return !m_stopping.isEmpty(); }

    [[nodiscard]] qint64 clockOffsetSecs(const QUuid& account) const { return m_clockOffsets.value(account); }

signals:
    void sessionStarted(const QUuid& account);
    // The session is gone (replaced, account removed, shutdown); requests
    // made through it are cancelled.
    void sessionStopped(const QUuid& account);
    void clockSkewDetected(const QUuid& account, qint64 offsetSecs);
    void capabilityLearned(const QUuid& account, const QString& bucket, const QString& featureId);
    void allStopped();

private:
    struct Entry {
        Account account;   // as the session was built — to notice changed settings
        std::unique_ptr<s3::Session> session;
    };

    void onAccountsChanged_();
    void stop_(const QUuid& account);
    void maybeAllStopped_();
    [[nodiscard]] static bool sameConnection_(const Account& a, const Account& b);

    AccountStore* m_accounts;
    SecretStore* m_secrets;
    std::map<QUuid, Entry> m_sessions;
    QSet<s3::Session*> m_stopping;   // shutdown() called, shutdownFinished pending
    QHash<QUuid, qint64> m_clockOffsets;
    bool m_shutDown = false;
    bool m_allStoppedSent = false;
};

} // namespace qbucket
