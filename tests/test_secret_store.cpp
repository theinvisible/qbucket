#include "accounts/QtKeychainBackend.h"
#include "accounts/SecretStore.h"
#include "support/FakeKeychainBackend.h"

#include <QSignalSpy>
#include <QtTest>

#include <optional>

using namespace qbucket;
using Outcome = SecretStore::Result::Outcome;

namespace {

const QString kSecret = QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYsecret");

QStringList g_log;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    g_log.append(msg);
}

SecretStore::Result await(const std::function<void(SecretStore::Callback)>& start)
{
    std::optional<SecretStore::Result> out;
    start([&out](const SecretStore::Result& r) { out = r; });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, 10000))
        return {};
    return *out;
}

} // namespace

class TestSecretStore : public QObject {
    Q_OBJECT

private slots:
    void writeReadRemove()
    {
        auto* backend = new testing::FakeKeychainBackend;
        SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
        const QUuid id = QUuid::createUuid();
        QObject ctx;

        SecretStore::Result r = await([&](auto cb) { store.write(id, SecretStore::Kind::SecretAccessKey, kSecret, &ctx, cb); });
        QVERIFY(r.ok());
        QVERIFY(!r.sessionOnly);
        QCOMPARE(backend->entries.value(QStringLiteral("QBucket|") + SecretStore::keyFor(id, SecretStore::Kind::SecretAccessKey)), kSecret);

        r = await([&](auto cb) { store.read(id, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        QVERIFY(r.ok());
        QCOMPARE(r.secret, kSecret);

        QVERIFY(await([&](auto cb) { store.write(id, SecretStore::Kind::SessionToken, QStringLiteral("tok"), &ctx, cb); }).ok());
        QVERIFY(await([&](auto cb) { store.removeAll(id, &ctx, cb); }).ok());
        QVERIFY(backend->entries.isEmpty());

        QSignalSpy needed(&store, &SecretStore::secretNeeded);
        r = await([&](auto cb) { store.read(id, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        QCOMPARE(r.outcome, Outcome::NotFound);
        QCOMPARE(needed.size(), 1);
    }

    void keysAreNamespacedPerAccountAndKind()
    {
        const QUuid id = QUuid::fromString(QStringLiteral("{12345678-1234-1234-1234-123456789abc}"));
        QCOMPARE(SecretStore::keyFor(id, SecretStore::Kind::SecretAccessKey),
                 QStringLiteral("12345678-1234-1234-1234-123456789abc/secretAccessKey"));
        QVERIFY(SecretStore::keyFor(id, SecretStore::Kind::SessionToken)
                != SecretStore::keyFor(id, SecretStore::Kind::SecretAccessKey));
    }

    void noKeychainMeansSessionOnlyNeverPlaintext()
    {
        auto* backend = new testing::FakeKeychainBackend;
        backend->mode = KeychainBackend::Status::NoBackend;
        SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
        QSignalSpy entered(&store, &SecretStore::sessionOnlyModeEntered);
        QSignalSpy needed(&store, &SecretStore::secretNeeded);
        const QUuid id = QUuid::createUuid();
        QObject ctx;

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no keychain available")));
        SecretStore::Result r = await([&](auto cb) { store.write(id, SecretStore::Kind::SecretAccessKey, kSecret, &ctx, cb); });
        QVERIFY(r.ok());
        QVERIFY(r.sessionOnly);        // the UI says why
        QVERIFY(store.isSessionOnly());
        QCOMPARE(entered.size(), 1);

        // Readable for this session, from memory — the keychain isn't asked again.
        const int readsBefore = backend->reads;
        r = await([&](auto cb) { store.read(id, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        QVERIFY(r.ok());
        QCOMPARE(r.secret, kSecret);
        QCOMPARE(backend->reads, readsBefore);

        // A secret not given this session: ask the user (once).
        const QUuid other = QUuid::createUuid();
        r = await([&](auto cb) { store.read(other, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        QCOMPARE(r.outcome, Outcome::Unavailable);
        QCOMPARE(needed.size(), 1);
        store.setSessionSecret(other, SecretStore::Kind::SecretAccessKey, QStringLiteral("typed"));
        QCOMPARE(await([&](auto cb) { store.read(other, SecretStore::Kind::SecretAccessKey, &ctx, cb); }).secret,
                 QStringLiteral("typed"));
        QCOMPARE(entered.size(), 1);
    }

    void lockedKeychainIsAnErrorNotAFallback()
    {
        auto* backend = new testing::FakeKeychainBackend;
        backend->mode = KeychainBackend::Status::AccessDenied;
        SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
        QObject ctx;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("keychain write failed")));
        const SecretStore::Result r = await([&](auto cb) {
            store.write(QUuid::createUuid(), SecretStore::Kind::SecretAccessKey, kSecret, &ctx, cb);
        });
        QCOMPARE(r.outcome, Outcome::Failed);
        QVERIFY(!store.isSessionOnly());
    }

    void callbacksNeedTheirContext()
    {
        auto* backend = new testing::FakeKeychainBackend;
        SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
        auto* ctx = new QObject;
        bool called = false;
        store.read(QUuid::createUuid(), SecretStore::Kind::SecretAccessKey, ctx,
                   [&](const SecretStore::Result&) { called = true; });
        delete ctx;
        QTest::qWait(50);
        QVERIFY(!called);
    }

    void secretsNeverReachTheLog()
    {
        g_log.clear();
        const QtMessageHandler previous = qInstallMessageHandler(captureLog);
        for (const KeychainBackend::Status mode : {KeychainBackend::Status::Ok, KeychainBackend::Status::Failed,
                                                   KeychainBackend::Status::NoBackend}) {
            auto* backend = new testing::FakeKeychainBackend;
            backend->mode = mode;
            SecretStore store{std::unique_ptr<KeychainBackend>(backend)};
            QObject ctx;
            const QUuid id = QUuid::createUuid();
            (void)await([&](auto cb) { store.write(id, SecretStore::Kind::SecretAccessKey, kSecret, &ctx, cb); });
            (void)await([&](auto cb) { store.read(id, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        }
        qInstallMessageHandler(previous);
        QVERIFY(!g_log.isEmpty());   // something was logged …
        for (const QString& line : std::as_const(g_log))
            QVERIFY2(!line.contains(kSecret), qPrintable(line));   // … but never the secret
    }

    // Against the real keychain of this machine — opt-in, it touches it.
    void realKeychainSmokeTest()
    {
        if (qEnvironmentVariable("QBUCKET_TEST_REAL_KEYCHAIN") != QLatin1String("1"))
            QSKIP("set QBUCKET_TEST_REAL_KEYCHAIN=1 to test against the system keychain");
        SecretStore store{std::make_unique<QtKeychainBackend>()};
        QObject ctx;
        const QUuid id = QUuid::createUuid();
        SecretStore::Result r = await([&](auto cb) { store.write(id, SecretStore::Kind::SecretAccessKey, kSecret, &ctx, cb); });
        QVERIFY2(r.ok(), qPrintable(r.message));
        if (r.sessionOnly)
            QSKIP("no keychain backend on this system (session-only mode works)");
        r = await([&](auto cb) { store.read(id, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        QVERIFY2(r.ok(), qPrintable(r.message));
        QCOMPARE(r.secret, kSecret);
        QVERIFY(await([&](auto cb) { store.removeAll(id, &ctx, cb); }).ok());
        r = await([&](auto cb) { store.read(id, SecretStore::Kind::SecretAccessKey, &ctx, cb); });
        QCOMPARE(r.outcome, Outcome::NotFound);
    }
};

QTEST_GUILESS_MAIN(TestSecretStore)
#include "test_secret_store.moc"
