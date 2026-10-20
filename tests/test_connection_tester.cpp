#include "controllers/ConnectionTester.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;

namespace {

s3::Credentials creds()
{
    s3::Credentials c;
    c.accessKeyId = QStringLiteral("AKIDDIALOG");
    c.secretAccessKey = QStringLiteral("dialog-secret");
    return c;
}

Account accountFor(const FakeS3Server& server)
{
    Account a;
    a.id = QUuid::createUuid();
    a.name = QStringLiteral("local");
    a.providerId = QStringLiteral("generic");
    a.region = QStringLiteral("test-1");
    a.endpoint = server.endpointUrl().toString();
    a.accessKeyId = creds().accessKeyId;
    return a;
}

const QByteArray kTwoBuckets = "<ListAllMyBucketsResult><Buckets>"
                               "<Bucket><Name>a</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                               "<Bucket><Name>b</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                               "</Buckets></ListAllMyBucketsResult>";

} // namespace

class TestConnectionTester : public QObject {
    Q_OBJECT

private slots:
    void succeeds()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(creds(), "test-1");
        server.on("GET", QStringLiteral("^/$"), FakeResponse::ok(kTwoBuckets));
        ConnectionTester tester;
        QSignalSpy done(&tester, &ConnectionTester::finished);
        tester.start(accountFor(server), creds().secretAccessKey);
        QVERIFY(tester.isRunning());
        QVERIFY(done.wait(10000));
        QCOMPARE(done.first().at(0).toBool(), true);
        QVERIFY(done.first().at(1).toString().contains(QLatin1String("2")));
        QVERIFY(!tester.isRunning());
    }

    void wrongSecret()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(creds(), "test-1");
        ConnectionTester tester;
        QSignalSpy done(&tester, &ConnectionTester::finished);
        tester.start(accountFor(server), QStringLiteral("wrong"));
        QVERIFY(done.wait(10000));
        QCOMPARE(done.first().at(0).toBool(), false);
        QCOMPARE(done.first().at(1).toString(), ConnectionTester::tr("The access key or the secret key is wrong."));
    }

    void externalBucketWhenListingIsForbidden()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("GET", QStringLiteral("^/$"), FakeResponse::s3Error(403, "AccessDenied"));
        server.on("HEAD", QStringLiteral("^/shared$"), FakeResponse());
        ConnectionTester tester;
        QSignalSpy done(&tester, &ConnectionTester::finished);
        Account a = accountFor(server);
        a.externalBuckets = {QStringLiteral("shared")};
        tester.start(a, creds().secretAccessKey);
        QVERIFY(done.wait(10000));
        QCOMPARE(done.first().at(0).toBool(), true);
        QVERIFY(done.first().at(1).toString().contains(QLatin1String("shared")));
    }

    void invalidEndpoint()
    {
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("x");
        a.providerId = QStringLiteral("generic");
        a.endpoint = QStringLiteral("ftp://nope");
        a.accessKeyId = QStringLiteral("k");
        ConnectionTester tester;
        QSignalSpy done(&tester, &ConnectionTester::finished);
        tester.start(a, QStringLiteral("s"));
        QCOMPARE(done.size(), 0);   // asynchronous
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first().at(0).toBool(), false);
    }

    void cancelledRunNeverTouchesTheNextOne()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        FakeResponse slow = FakeResponse::ok(kTwoBuckets);
        slow.delayMs = 300;
        server.on("GET", QString(), slow);
        ConnectionTester tester;
        QSignalSpy done(&tester, &ConnectionTester::finished);
        tester.start(accountFor(server), creds().secretAccessKey);
        tester.cancel();
        tester.start(accountFor(server), creds().secretAccessKey);
        QVERIFY(done.wait(10000));
        QTest::qWait(400);
        QCOMPARE(done.size(), 1);                 // only the second run reports
        QCOMPARE(done.first().at(0).toBool(), true);
    }

    void sessionOptionsUseTheProvider()
    {
        Account a;
        a.providerId = QStringLiteral("hetzner");
        a.region = QStringLiteral("nbg1");
        const s3::SessionOptions o = ConnectionTester::sessionOptionsFor(a, nullptr);
        QCOMPARE(o.profile->id(), QStringLiteral("hetzner"));
        QCOMPARE(o.endpoint.url, QUrl(QStringLiteral("https://nbg1.your-objectstorage.com")));
        QCOMPARE(o.endpoint.region, QStringLiteral("nbg1"));

        a.endpoint = QStringLiteral("https://custom.example.com");
        QCOMPARE(ConnectionTester::sessionOptionsFor(a, nullptr).endpoint.url,
                 QUrl(QStringLiteral("https://custom.example.com")));

        Account g;
        g.providerId = QStringLiteral("unknown-provider");
        QCOMPARE(ConnectionTester::sessionOptionsFor(g, nullptr).profile->id(), QStringLiteral("generic"));
    }
};

QTEST_GUILESS_MAIN(TestConnectionTester)
#include "test_connection_tester.moc"
