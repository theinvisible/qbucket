#include "accounts/KeychainCredentialsProvider.h"

#include <QCoreApplication>

namespace qbucket {

std::shared_ptr<KeychainCredentialsProvider> KeychainCredentialsProvider::create(SecretStore* store,
                                                                                 const Account& account)
{
    return std::shared_ptr<KeychainCredentialsProvider>(new KeychainCredentialsProvider(store, account));
}

KeychainCredentialsProvider::KeychainCredentialsProvider(SecretStore* store, const Account& account)
    : m_store(store),
      m_accountId(account.id),
      m_accessKeyId(account.accessKeyId)
{
}

void KeychainCredentialsProvider::fetch(Callback done)
{
    const auto fail = [](const QString& message) {
        return s3::Error::make(s3::ErrorCategory::Credentials, message);
    };

    // To the GUI thread through the application object, which outlives
    // every session; everything else is touched only there.
    const std::weak_ptr<KeychainCredentialsProvider> weak = weak_from_this();
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [weak, done = std::move(done), fail] {
            const std::shared_ptr<KeychainCredentialsProvider> self = weak.lock();
            if (!self || !self->m_store) {
                done(fail(QStringLiteral("the secret store is no longer available")));
                return;
            }
            const QString keyId = self->m_accessKeyId;
            self->m_store->read(
                self->m_accountId, SecretStore::Kind::SecretAccessKey, QCoreApplication::instance(),
                [done, keyId, fail](const SecretStore::Result& r) {
                    if (!r.ok()) {
                        done(fail(r.outcome == SecretStore::Result::Outcome::NotFound
                                      ? QStringLiteral("no secret access key stored for this account")
                                      : r.message));
                        return;
                    }
                    s3::Credentials c;
                    c.accessKeyId = keyId;
                    c.secretAccessKey = r.secret;
                    done(c);
                });
        },
        Qt::QueuedConnection);
}

} // namespace qbucket
