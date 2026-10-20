#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "support/FakeKeychainBackend.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;

namespace {

const QString kKeyId = QStringLiteral("AKIDSESSIONS");
const QString kSecret = QStringLiteral("sessions-secret");

const QByteArray kOneBucket = "<ListAllMyBucketsResult><Buckets>"
                              "<Bucket><Name>a</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                              "</Buckets></ListAllMyBucketsResult>";

s3::Credentials creds()
{
    s3::Credentials c;
    c.accessKeyId = kKeyId;
    c.secretAccessKey = kSecret;
    return c;
}

} // namespace

class TestAccountSessions : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_server = std::make_unique<FakeS3Server>();
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(creds(), "test-1");
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(kOneBucket));

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run)));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        m_keychain = backend.get();
        m_secrets = std::make_unique<SecretStore>(std::move(backend));

        m_account = Account{};
        m_account.id = QUuid::createUuid();
        m_account.name = QStringLiteral("local");
        m_account.providerId = QStringLiteral("generic");
        m_account.region = QStringLiteral("test-1");
        m_account.endpoint = m_server->endpointUrl().toString();
        m_account.accessKeyId = kKeyId;
        QVERIFY(m_accounts->save(m_account));
        m_keychain->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                       + SecretStore::keyFor(m_account.id, SecretStore::Kind::SecretAccessKey),
                                   kSecret);
    }

    void cleanup()
    {
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void createsOneSessionPerAccountLazily()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QSignalSpy started(&sessions, &AccountSessions::sessionStarted);
        QVERIFY(!sessions.hasSession(m_account.id));

        const s3::Result<s3::Session*> s = sessions.session(m_account.id);
        QVERIFY(s);
        QCOMPARE(started.size(), 1);
        QVERIFY(sessions.hasSession(m_account.id));

        // The keychain secret signs the request (the server verifies it).
        QObject context;
        std::optional<s3::Result<s3::BucketList>> listed;
        (*s)->listBuckets(&context, [&](s3::Result<s3::BucketList> r) { listed.emplace(std::move(r)); });
        QTRY_VERIFY_WITH_TIMEOUT(listed.has_value(), 10000);
        if (!*listed)
            QFAIL(qPrintable(listed->error().toString()));
        QCOMPARE((*listed)->buckets.size(), 1);

        const s3::Result<s3::Session*> again = sessions.session(m_account.id);
        QVERIFY(again);
        QCOMPARE(*again, *s);
        QCOMPARE(started.size(), 1);
    }

    void refusesUnknownAccountsAndBadEndpoints()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        const s3::Result<s3::Session*> unknown = sessions.session(QUuid::createUuid());
        QVERIFY(!unknown);
        QCOMPARE(unknown.error().category, s3::ErrorCategory::InvalidRequest);

        Account bad = m_account;
        bad.endpoint = QStringLiteral("ftp://example.com");
        QVERIFY(m_accounts->save(bad));
        const s3::Result<s3::Session*> r = sessions.session(bad.id);
        QVERIFY(!r);
        QCOMPARE(r.error().category, s3::ErrorCategory::InvalidRequest);
        QVERIFY(!sessions.hasSession(bad.id));
    }

    void changedConnectionSettingsReplaceTheSession()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QVERIFY(sessions.session(m_account.id));
        QSignalSpy stopped(&sessions, &AccountSessions::sessionStopped);

        // A new name doesn't touch the connection.
        Account renamed = m_account;
        renamed.name = QStringLiteral("renamed");
        QVERIFY(m_accounts->save(renamed));
        QCOMPARE(stopped.size(), 0);
        QVERIFY(sessions.hasSession(m_account.id));

        Account moved = renamed;
        moved.region = QStringLiteral("test-2");
        QVERIFY(m_accounts->save(moved));
        QCOMPARE(stopped.size(), 1);
        QCOMPARE(stopped.first().at(0).toUuid(), m_account.id);
        QVERIFY(!sessions.hasSession(m_account.id));
        QVERIFY(sessions.isStopping());
        QTRY_VERIFY(!sessions.isStopping());

        const s3::Result<s3::Session*> fresh = sessions.session(m_account.id);
        QVERIFY(fresh);
        QCOMPARE((*fresh)->options().endpoint.region, QStringLiteral("test-2"));
    }

    void removedAccountStopsItsSession()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QVERIFY(sessions.session(m_account.id));
        QSignalSpy stopped(&sessions, &AccountSessions::sessionStopped);
        QVERIFY(m_accounts->remove(m_account.id));
        QCOMPARE(stopped.size(), 1);
        QVERIFY(!sessions.session(m_account.id));
    }

    void reconnectBuildsANewSession()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QVERIFY(sessions.session(m_account.id));
        QSignalSpy stopped(&sessions, &AccountSessions::sessionStopped);
        sessions.reconnect(m_account.id);
        QCOMPARE(stopped.size(), 1);
        QVERIFY(!sessions.hasSession(m_account.id));
        QVERIFY(sessions.session(m_account.id));
    }

    void runningRequestsAreCancelledWhenTheSessionStops()
    {
        m_server->clearRoutes();
        FakeResponse slow = FakeResponse::ok(kOneBucket);
        slow.delayMs = 5000;
        m_server->on("GET", QStringLiteral("^/$"), slow);

        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        const s3::Result<s3::Session*> s = sessions.session(m_account.id);
        QVERIFY(s);
        QObject context;
        std::optional<s3::Result<s3::BucketList>> listed;
        (*s)->listBuckets(&context, [&](s3::Result<s3::BucketList> r) { listed.emplace(std::move(r)); });
        sessions.reconnect(m_account.id);
        QTRY_VERIFY_WITH_TIMEOUT(listed.has_value(), 3000);
        QVERIFY(!*listed);
        QCOMPARE(listed->error().category, s3::ErrorCategory::Cancelled);
    }

    void shutdownStopsEverything()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QVERIFY(sessions.session(m_account.id));
        QSignalSpy all(&sessions, &AccountSessions::allStopped);
        sessions.shutdown();
        QVERIFY(sessions.isShutDown());
        QVERIFY(all.wait(5000));
        QTest::qWait(50);
        QCOMPARE(all.size(), 1);   // exactly once

        const s3::Result<s3::Session*> after = sessions.session(m_account.id);
        QVERIFY(!after);
        QCOMPARE(after.error().category, s3::ErrorCategory::Cancelled);
    }

    void shutdownWithoutSessionsStillReports()
    {
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QSignalSpy all(&sessions, &AccountSessions::allStopped);
        sessions.shutdown();
        QCOMPARE(all.size(), 0);   // queued, never inside the call
        QVERIFY(all.wait(2000));
    }

    void clockOffsetIsHandedToTheNextSession()
    {
        m_server->setClockOffsetSecs(qint64(30) * 60);
        AccountSessions sessions(m_accounts.get(), m_secrets.get());
        QSignalSpy skew(&sessions, &AccountSessions::clockSkewDetected);
        const s3::Result<s3::Session*> s = sessions.session(m_account.id);
        QVERIFY(s);
        QObject context;
        bool done = false;
        (*s)->listBuckets(&context, [&](const s3::Result<s3::BucketList>&) { done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QTRY_VERIFY(!skew.isEmpty());
        const qint64 offset = skew.last().at(1).toLongLong();
        QVERIFY(qAbs(offset - qint64(30) * 60) < 120);
        QCOMPARE(sessions.clockOffsetSecs(m_account.id), offset);

        sessions.reconnect(m_account.id);
        const s3::Result<s3::Session*> next = sessions.session(m_account.id);
        QVERIFY(next);
        QCOMPARE((*next)->options().initialClockOffsetSecs, offset);
    }

    void destructorCleansUpWhileStopping()
    {
        // ASan/LSan: a session still stopping is deleted with its owner.
        auto sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
        QVERIFY(sessions->session(m_account.id));
        sessions->reconnect(m_account.id);
        QVERIFY(sessions->session(m_account.id));
        sessions.reset();
    }

private:
    QTemporaryDir m_dir;
    int m_run = 0;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    testing::FakeKeychainBackend* m_keychain = nullptr;
    std::unique_ptr<SecretStore> m_secrets;
    Account m_account;
};

QTEST_GUILESS_MAIN(TestAccountSessions)
#include "test_account_sessions.moc"
