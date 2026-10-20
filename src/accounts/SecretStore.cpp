#include "accounts/SecretStore.h"

#include "diagnostics/Logging.h"

#include <QTimer>

namespace qbucket {
namespace {

QString kindName(SecretStore::Kind kind)
{
    switch (kind) {
    case SecretStore::Kind::SecretAccessKey: return QStringLiteral("secretAccessKey");
    case SecretStore::Kind::SessionToken:    return QStringLiteral("sessionToken");
    case SecretStore::Kind::ProxyPassword:   return QStringLiteral("proxyPassword");
    }
    return QStringLiteral("unknown");
}

SecretStore::Result outcome(SecretStore::Result::Outcome o, const QString& message = {})
{
    SecretStore::Result r;
    r.outcome = o;
    r.message = message;
    return r;
}

} // namespace

SecretStore::SecretStore(std::unique_ptr<KeychainBackend> backend, QObject* parent)
    : QObject(parent),
      m_backend(std::move(backend))
{
    Q_ASSERT(m_backend);
}

SecretStore::~SecretStore() = default;

QString SecretStore::keyFor(const QUuid& account, Kind kind)
{
    return account.toString(QUuid::WithoutBraces) + u'/' + kindName(kind);
}

void SecretStore::deliver_(const QPointer<QObject>& context, const Callback& done, Result result)
{
    if (!context)
        return;
    QTimer::singleShot(0, context.data(), [done, result = std::move(result)] { done(result); });
}

void SecretStore::enterSessionOnly_(const QString& reason)
{
    if (m_sessionOnly)
        return;
    m_sessionOnly = true;
    m_sessionOnlyReason = reason;
    qCWarning(lcApp).noquote() << "no keychain available, secrets are kept for this session only:" << reason;
    emit sessionOnlyModeEntered(reason);
}

void SecretStore::read(const QUuid& account, Kind kind, QObject* context, Callback done)
{
    const QString key = keyFor(account, kind);
    const QPointer<QObject> ctx(context);
    if (const auto it = m_memory.constFind(key); it != m_memory.constEnd()) {
        Result r = outcome(Result::Outcome::Ok);
        r.secret = *it;
        deliver_(ctx, done, r);
        return;
    }
    if (m_sessionOnly) {
        deliver_(ctx, done, outcome(Result::Outcome::Unavailable, m_sessionOnlyReason));
        emit secretNeeded(account, kind);
        return;
    }
    const QPointer<SecretStore> self(this);
    m_backend->read(QString::fromLatin1(kService), key,
                    [self, ctx, done = std::move(done), account, kind](KeychainBackend::Status s, const QString& secret,
                                                     const QString& error) {
        if (!self) {
            deliver_(ctx, done, outcome(Result::Outcome::Failed, QStringLiteral("secret store closed")));
            return;
        }
        switch (s) {
        case KeychainBackend::Status::Ok: {
            Result r = outcome(Result::Outcome::Ok);
            r.secret = secret;
            deliver_(ctx, done, r);
            return;
        }
        case KeychainBackend::Status::NotFound:
            deliver_(ctx, done, outcome(Result::Outcome::NotFound, QStringLiteral("no secret stored")));
            emit self->secretNeeded(account, kind);
            return;
        case KeychainBackend::Status::NoBackend:
            self->enterSessionOnly_(error);
            deliver_(ctx, done, outcome(Result::Outcome::Unavailable, error));
            emit self->secretNeeded(account, kind);
            return;
        case KeychainBackend::Status::AccessDenied:
        case KeychainBackend::Status::Failed:
            qCWarning(lcApp).noquote() << "keychain read failed for" << keyFor(account, kind) << ':' << error;
            deliver_(ctx, done, outcome(Result::Outcome::Failed, error));
            return;
        }
    });
}

void SecretStore::write(const QUuid& account, Kind kind, const QString& secret, QObject* context,
                        Callback done)
{
    const QString key = keyFor(account, kind);
    const QPointer<QObject> ctx(context);
    if (m_sessionOnly) {
        m_memory.insert(key, secret);
        Result r = outcome(Result::Outcome::Ok, m_sessionOnlyReason);
        r.sessionOnly = true;
        deliver_(ctx, done, r);
        return;
    }
    const QPointer<SecretStore> self(this);
    m_backend->write(QString::fromLatin1(kService), key, secret,
                     [self, ctx, done = std::move(done), key, secret](KeychainBackend::Status s, const QString& error) {
        if (!self) {
            deliver_(ctx, done, outcome(Result::Outcome::Failed, QStringLiteral("secret store closed")));
            return;
        }
        if (s == KeychainBackend::Status::Ok) {
            self->m_memory.remove(key);
            deliver_(ctx, done, outcome(Result::Outcome::Ok));
            return;
        }
        if (s == KeychainBackend::Status::NoBackend) {
            self->enterSessionOnly_(error);
            self->m_memory.insert(key, secret);
            Result r = outcome(Result::Outcome::Ok, error);
            r.sessionOnly = true;
            deliver_(ctx, done, r);
            return;
        }
        qCWarning(lcApp).noquote() << "keychain write failed for" << key << ':' << error;
        deliver_(ctx, done, outcome(Result::Outcome::Failed, error));
    });
}

void SecretStore::removeAll(const QUuid& account, QObject* context, Callback done)
{
    const QPointer<QObject> ctx(context);
    const QList<Kind> kinds = {Kind::SecretAccessKey, Kind::SessionToken, Kind::ProxyPassword};
    for (const Kind k : kinds)
        m_memory.remove(keyFor(account, k));
    if (m_sessionOnly) {
        deliver_(ctx, done, outcome(Result::Outcome::Ok));
        return;
    }

    // All kinds at once; the first failure is reported, the others still tried.
    struct State {
        int pending = 0;
        QString firstError;
        Callback done;
    };
    auto state = std::make_shared<State>();
    state->pending = int(kinds.size());
    state->done = std::move(done);
    for (const Kind k : kinds) {
        m_backend->remove(QString::fromLatin1(kService), keyFor(account, k),
                          [state, ctx](KeychainBackend::Status s, const QString& error) {
            if (s != KeychainBackend::Status::Ok && s != KeychainBackend::Status::NoBackend
                && state->firstError.isEmpty())
                state->firstError = error.isEmpty() ? QStringLiteral("keychain error") : error;
            if (--state->pending == 0) {
                deliver_(ctx, state->done, state->firstError.isEmpty()
                                        ? outcome(Result::Outcome::Ok)
                                        : outcome(Result::Outcome::Failed, state->firstError));
            }
        });
    }
}

void SecretStore::setSessionSecret(const QUuid& account, Kind kind, const QString& secret)
{
    m_memory.insert(keyFor(account, kind), secret);
}

} // namespace qbucket
