#include "client/S3Client.h"

#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QElapsedTimer>
#include <QMutex>
#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

Credentials testCredentials()
{
    Credentials c;
    c.accessKeyId = QStringLiteral("AKIDTEST");
    c.secretAccessKey = QStringLiteral("test/secret+key");
    return c;
}

class CountingObserver final : public RequestObserver {
public:
    void onRequestFinished(const RequestRecord& r) override
    {
        QMutexLocker lock(&mutex);
        attempts.append(r.attempt);
    }
    QMutex mutex;
    QList<int> attempts;
};

template <typename T, typename Start>
Result<T> await(Start start, int timeoutMs = 15000)
{
    std::optional<Result<T>> out;
    start([&out](Result<T> r) { out.emplace(std::move(r)); });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, timeoutMs))
        return Error::make(ErrorCategory::Internal, QStringLiteral("test timeout"));
    return std::move(*out);
}

const QByteArray kBuckets = "<ListAllMyBucketsResult><Buckets/></ListAllMyBucketsResult>";
const QByteArray kXml = "application/xml";

FakeResponse slowDown()
{
    return FakeResponse::s3Error(503, "SlowDown", "Please reduce your request rate.");
}

} // namespace

// The §12.6 fault matrix against the real client: what gets retried, what
// never does, and how clock skew and wrong regions are repaired.
class TestS3ClientFaults : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::shared_ptr<CountingObserver> m_observer;

    std::unique_ptr<S3Client> makeClient(const QUrl& url = {}, const QString& region = QStringLiteral("test-1"),
                                         int transferTimeoutMs = 5000)
    {
        ClientConfig cfg;
        cfg.endpoint.url = url.isValid() ? url : m_server->endpointUrl();
        cfg.endpoint.region = region;
        cfg.profile = std::make_shared<GenericS3Profile>();
        cfg.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        cfg.observer = m_observer;
        cfg.transferTimeoutMs = transferTimeoutMs;
        cfg.retry.maxAttempts = 4;
        cfg.retry.baseDelayMs = 10;
        cfg.retry.maxDelayMs = 50;
        return std::make_unique<S3Client>(cfg);
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_observer = std::make_shared<CountingObserver>();
    }

    void cleanup()
    {
        delete m_server;
        m_server = nullptr;
    }

    void retriesSlowDownThenSucceeds()
    {
        Matcher any;
        m_server->addRoute(any, {slowDown(), slowDown(), FakeResponse::ok(kBuckets, kXml)});
        auto client = makeClient();
        const Result<BucketList> r = await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(m_server->requestCount(), 3);
        QMutexLocker lock(&m_observer->mutex);
        QCOMPARE(m_observer->attempts, (QList<int>{1, 2, 3}));
    }

    void givesUpAfterMaxAttempts()
    {
        m_server->on("", QString(), FakeResponse::s3Error(500, "InternalError"));
        auto client = makeClient();
        const Result<BucketList> r = await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Transient);
        QCOMPARE(r.error().code, QStringLiteral("InternalError"));
        QCOMPARE(m_server->requestCount(), 4);
    }

    void permanentErrorsAreNotRetried()
    {
        m_server->on("", QString(), FakeResponse::s3Error(404, "NoSuchBucket"));
        auto client = makeClient();
        QVERIFY(!await<void>([&](auto cb) { client->headBucket(QStringLiteral("b"), std::move(cb)); }));
        QCOMPARE(m_server->requestCount(), 1);
    }

    void networkFaultsOnIdempotentRequests_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("dropped") << 0;
        QTest::newRow("truncated body") << 1;
        QTest::newRow("wrong content-length") << 2;
        QTest::newRow("stalled") << 3;
    }

    void networkFaultsOnIdempotentRequests()
    {
        QFETCH(int, fault);
        FakeResponse bad = FakeResponse::ok(kBuckets, kXml);
        switch (fault) {
        case 0: bad.dropWithoutResponse = true; break;
        case 1: bad.closeAfterBodyBytes = 10; break;
        case 2: bad.declaredContentLength = kBuckets.size() + 20; bad.closeConnection = true; break;
        case 3: bad.stallAfterBodyBytes = 5; break;
        }
        Matcher any;
        m_server->addRoute(any, {bad, FakeResponse::ok(kBuckets, kXml)});
        auto client = makeClient({}, QStringLiteral("test-1"), 400);
        const Result<BucketList> r = await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(m_server->requestCount(), 2);
    }

    void lostReplyOfNonIdempotentRequestIsNeverRetried()
    {
        FakeResponse drop;
        drop.dropWithoutResponse = true;
        m_server->on("", QString(), drop);
        auto client = makeClient();
        CreateBucketParams p;
        p.bucket = QStringLiteral("maybe-created");
        const Result<void> r = await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Transient);
        QVERIFY(r.error().outcomeUnknown);
        QCOMPARE(m_server->requestCount(), 1);
    }

    void serverErrorOnNonIdempotentRequestIsNotRetried()
    {
        m_server->on("", QString(), slowDown());
        auto client = makeClient();
        CreateBucketParams p;
        p.bucket = QStringLiteral("busy");
        QVERIFY(!await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); }));
        QCOMPARE(m_server->requestCount(), 1);
    }

    void unreachableServerIsRetriedEvenForNonIdempotent()
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const QUrl dead(QStringLiteral("http://127.0.0.1:%1").arg(probe.serverPort()));
        probe.close();   // nothing listens there now: connection refused

        auto client = makeClient(dead);
        CreateBucketParams p;
        p.bucket = QStringLiteral("never-sent");
        const Result<void> r = await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Transient);
        QVERIFY(!r.error().outcomeUnknown);
        QMutexLocker lock(&m_observer->mutex);
        QCOMPARE(m_observer->attempts.size(), 4);
    }

    void retryAfterIsHonoured()
    {
        Matcher any;
        m_server->addRoute(any, {slowDown().withHeader("Retry-After", "1"), FakeResponse::ok(kBuckets, kXml)});
        auto client = makeClient();
        QElapsedTimer t;
        t.start();
        QVERIFY(await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); }));
        QVERIFY(t.elapsed() >= 950);
    }

    void abortDuringBackoff()
    {
        m_server->on("", QString(), slowDown().withHeader("Retry-After", "30"));
        auto client = makeClient();
        std::optional<Result<BucketList>> out;
        const quint64 id = client->listBuckets([&](Result<BucketList> r) { out.emplace(std::move(r)); });
        QTRY_COMPARE(m_server->requestCount(), 1);
        QTest::qWait(50);   // now waiting for the 30 s Retry-After
        client->abort(id);
        QTRY_VERIFY_WITH_TIMEOUT(out.has_value(), 2000);
        QCOMPARE(out->error().category, ErrorCategory::Cancelled);
        QCOMPARE(m_server->requestCount(), 1);
    }

    void clockSkewIsCorrectedOnce()
    {
        m_server->setClockOffsetSecs(20 * 60);
        m_server->on("", QString(), FakeResponse::ok(kBuckets, kXml));
        auto client = makeClient();
        QSignalSpy skew(client.get(), &S3Client::clockSkewDetected);

        // Non-idempotent on purpose: the server rejected before executing.
        CreateBucketParams p;
        p.bucket = QStringLiteral("skewed");
        const Result<void> r = await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(m_server->requestCount(), 2);
        QVERIFY(skew.size() >= 1);
        const qint64 offset = skew.first().first().toLongLong();
        QVERIFY(std::abs(offset - 1200) <= 5);

        // Later requests are signed right away.
        QVERIFY(await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); }));
        QCOMPARE(m_server->requestCount(), 3);
    }

    void uncorrectableClockSkewFails()
    {
        // No ServerTime and no usable Date: nothing to correct from.
        m_server->clearRoutes();
        FakeS3Server plain;
        QVERIFY(plain.listen());
        plain.on("", QString(),
                 FakeResponse::s3Error(403, "RequestTimeTooSkewed").withHeader("Date", "garbage"));
        auto client = makeClient(plain.endpointUrl());
        const Result<BucketList> r = await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::ClockSkew);
        QCOMPARE(plain.requestCount(), 1);
    }

    void wrongSigningRegionIsLearned()
    {
        // The server expects "eu-x"; the account says "test-1".
        m_server->setVerifySignatures(testCredentials(), "eu-x");
        m_server->on("", QString(), FakeResponse());
        auto client = makeClient();
        QVERIFY(await<void>([&](auto cb) { client->headBucket(QStringLiteral("far"), std::move(cb)); }));
        QCOMPARE(m_server->requestCount(), 2);
        QVERIFY(m_server->requests().last().header("Authorization").contains("/eu-x/s3/"));

        // Cached per bucket: no second detour.
        QVERIFY(await<void>([&](auto cb) { client->headBucket(QStringLiteral("far"), std::move(cb)); }));
        QCOMPARE(m_server->requestCount(), 3);
    }

    void permanentRedirectUsesRegionNotLocation()
    {
        FakeS3Server plain;   // no signature checks: we look at the redirect logic
        QVERIFY(plain.listen());
        FakeResponse moved = FakeResponse::s3Error(301, "PermanentRedirect")
                                 .withHeader("x-amz-bucket-region", "eu-y")
                                 .withHeader("Location", "http://evil.invalid/steal");
        Matcher any;
        plain.addRoute(any, {moved, FakeResponse()});
        auto client = makeClient(plain.endpointUrl());
        QVERIFY(await<void>([&](auto cb) { client->headBucket(QStringLiteral("moved"), std::move(cb)); }));
        QCOMPARE(plain.requestCount(), 2);
        QVERIFY(plain.requests().last().header("Authorization").contains("/eu-y/s3/"));
    }

    void redirectWithoutRegionFails()
    {
        FakeS3Server plain;
        QVERIFY(plain.listen());
        plain.on("", QString(), FakeResponse::s3Error(301, "PermanentRedirect"));
        auto client = makeClient(plain.endpointUrl());
        const Result<void> r = await<void>([&](auto cb) { client->headBucket(QStringLiteral("x"), std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Redirect);
        QCOMPARE(plain.requestCount(), 1);
    }
};

QTEST_GUILESS_MAIN(TestS3ClientFaults)
#include "test_s3client_faults.moc"
