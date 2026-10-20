#pragma once

#include "accounts/KeychainBackend.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUuid>

#include <functional>
#include <memory>

namespace qbucket {

// The only place secrets are kept (PLAN.md §1, CLAUDE.md): the operating
// system's keychain, service "QBucket", key "<account id>/<kind>". Never an
// INI, a JSON file, a log or the transfer resume store.
//
// Without a keychain backend (e.g. Linux without Secret Service) the store
// switches to session-only mode: secrets are kept in memory for this run
// and the user is asked once per session — never a plaintext fallback.
//
// Thread affinity: GUI thread. Callbacks are always asynchronous, at most
// once, and dropped if their context object is gone.
class SecretStore : public QObject {
    Q_OBJECT

public:
    enum class Kind { SecretAccessKey, SessionToken, ProxyPassword };
    Q_ENUM(Kind)

    struct Result {
        enum class Outcome { Ok, NotFound, Unavailable, Failed };
        Outcome outcome = Outcome::Failed;
        QString secret;          // Ok reads only
        QString message;         // English, for the UI / log — never contains the secret
        bool sessionOnly = false;   // write: kept in memory only (no keychain)
        [[nodiscard]] bool ok() const { return outcome == Outcome::Ok; }
    };
    using Callback = std::function<void(const Result&)>;

    static constexpr char kService[] = "QBucket";

    explicit SecretStore(std::unique_ptr<KeychainBackend> backend, QObject* parent = nullptr);
    ~SecretStore() override;

    void read(const QUuid& account, Kind kind, QObject* context, Callback done);
    void write(const QUuid& account, Kind kind, const QString& secret, QObject* context, Callback done);
    // All secrets of an account — when the account is deleted.
    void removeAll(const QUuid& account, QObject* context, Callback done);

    // The answer to secretNeeded in session-only mode (or for a secret the
    // keychain lost): held in memory until the app quits.
    void setSessionSecret(const QUuid& account, Kind kind, const QString& secret);

    [[nodiscard]] bool isSessionOnly() const { return m_sessionOnly; }
    [[nodiscard]] QString sessionOnlyReason() const { return m_sessionOnlyReason; }

    static QString keyFor(const QUuid& account, Kind kind);

signals:
    // No keychain backend: secrets live in memory for this session only.
    void sessionOnlyModeEntered(const QString& reason);
    // A read found no secret — the UI may ask the user for it.
    void secretNeeded(const QUuid& account, qbucket::SecretStore::Kind kind);

private:
    void enterSessionOnly_(const QString& reason);
    static void deliver_(const QPointer<QObject>& context, const Callback& done, Result result);

    std::unique_ptr<KeychainBackend> m_backend;
    QHash<QString, QString> m_memory;   // session-only secrets
    bool m_sessionOnly = false;
    QString m_sessionOnlyReason;
};

} // namespace qbucket
