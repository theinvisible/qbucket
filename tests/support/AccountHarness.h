#pragma once

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "FakeKeychainBackend.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QTemporaryDir>

#include <memory>

namespace qbucket::testing {

// One account against a FakeS3Server that checks every signature, with its
// secret in a fake keychain and an AccountSessions on top — what most app
// tests need. Check ok() after construction.
class AccountHarness {
public:
    static constexpr char kKeyId[] = "AKIDHARNESS";
    static constexpr char kSecret[] = "harness-secret";

    explicit AccountHarness(const QString& name = QStringLiteral("local"))
    {
        server = std::make_unique<s3::testing::FakeS3Server>();
        m_ok = dir.isValid() && server->listen();
        s3::Credentials c;
        c.accessKeyId = QString::fromLatin1(kKeyId);
        c.secretAccessKey = QString::fromLatin1(kSecret);
        server->setVerifySignatures(c, "test-1");

        accounts = std::make_unique<AccountStore>(dir.filePath(QStringLiteral("accounts.ini")));
        auto backend = std::make_unique<FakeKeychainBackend>();
        keychain = backend.get();
        Account a;
        a.id = QUuid::createUuid();
        a.name = name;
        a.providerId = QStringLiteral("generic");
        a.region = QStringLiteral("test-1");
        a.endpoint = server->endpointUrl().toString();
        a.accessKeyId = c.accessKeyId;
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                c.secretAccessKey);
        secrets = std::make_unique<SecretStore>(std::move(backend));
        m_ok = m_ok && accounts->save(a);
        account = a;
        sessions = std::make_unique<AccountSessions>(accounts.get(), secrets.get());
    }

    ~AccountHarness()
    {
        sessions.reset();   // before the secret store (KeychainCredentialsProvider)
        secrets.reset();
    }

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] QString path(const QString& name) const { return dir.filePath(name); }

    QTemporaryDir dir;
    std::unique_ptr<s3::testing::FakeS3Server> server;
    std::unique_ptr<AccountStore> accounts;
    FakeKeychainBackend* keychain = nullptr;
    std::unique_ptr<SecretStore> secrets;
    std::unique_ptr<AccountSessions> sessions;
    Account account;

private:
    bool m_ok = false;
};

} // namespace qbucket::testing
