#include "accounts/KeychainCredentialsProvider.h"
#include "support/FakeKeychainBackend.h"

#include <QtTest>

#include <optional>
#include <thread>

using namespace qbucket;

namespace {

Account account()
{
    Account a;
    a.id = QUuid::createUuid();
    a.name = QStringLiteral("test");
    a.providerId = QStringLiteral("hetzner");
    a.accessKeyId = QStringLiteral("AKIDKEYCHAIN");
    return a;
}

// Calls fetch() from a foreign thread, like the S3 worker does.
s3::Result<s3::Credentials> fetchFromWorker(s3::CredentialsProvider& provider)
{
    std::optional<s3::Result<s3::Credentials>> out;
    std::thread worker([&] { provider.fetch([&out](s3::Result<s3::Credentials> r) { out.emplace(std::move(r)); }); });
    worker.join();
    if (!QTest::qWaitFor([&] { return out.has_value(); }, 10000))
        return s3::Error::make(s3::ErrorCategory::Internal, QStringLiteral("timeout"));
    return std::move(*out);
}

} // namespace

class TestKeychainCredentialsProvider : public QObject {
    Q_OBJECT

private slots:
    void deliversTheKeychainSecret()
    {
        auto* backend = new testing::FakeKeychainBackend;
        SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
        const Account a = account();
        backend->entries.insert(QStringLiteral("QBucket|") + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                QStringLiteral("the-secret"));
        const auto provider = KeychainCredentialsProvider::create(&store, a);
        const s3::Result<s3::Credentials> c = fetchFromWorker(*provider);
        QVERIFY2(c, qPrintable(c ? QString() : c.error().toString()));
        QCOMPARE(c->accessKeyId, QStringLiteral("AKIDKEYCHAIN"));
        QCOMPARE(c->secretAccessKey, QStringLiteral("the-secret"));
    }

    void missingSecretIsACredentialsError()
    {
        auto* backend = new testing::FakeKeychainBackend;
        SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
        const auto provider = KeychainCredentialsProvider::create(&store, account());
        const s3::Result<s3::Credentials> c = fetchFromWorker(*provider);
        QVERIFY(!c);
        QCOMPARE(c.error().category, s3::ErrorCategory::Credentials);
    }

    void answersEvenWhenTheStoreIsGone()
    {
        auto store = std::make_unique<SecretStore>(std::make_unique<testing::FakeKeychainBackend>());
        const auto provider = KeychainCredentialsProvider::create(store.get(), account());
        store.reset();
        const s3::Result<s3::Credentials> c = fetchFromWorker(*provider);
        QVERIFY(!c);
        QCOMPARE(c.error().category, s3::ErrorCategory::Credentials);
    }
};

QTEST_GUILESS_MAIN(TestKeychainCredentialsProvider)
#include "test_keychain_credentials_provider.moc"
