#include <qbucket/s3/ListObjectsPager.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QElapsedTimer>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <optional>
#include <thread>

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

class ThreadRecorder final : public RequestObserver {
public:
    void onRequestFinished(const RequestRecord&) override
    {
        QMutexLocker lock(&mutex);
        threads.insert(QThread::currentThread());
    }
    QMutex mutex;
    QSet<QThread*> threads;
};

const QByteArray kXml = "application/xml";

QByteArray listPage(const QString& key, const QString& next)
{
    QByteArray body = "<ListBucketResult><Name>bkt</Name><IsTruncated>"
                    + QByteArray(next.isEmpty() ? "false" : "true") + "</IsTruncated>"
                    + "<Contents><Key>" + key.toUtf8() + "</Key></Contents>";
    if (!next.isEmpty())
        body += "<NextContinuationToken>" + next.toUtf8() + "</NextContinuationToken>";
    return body + "</ListBucketResult>";
}

} // namespace

class TestSession : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::shared_ptr<ThreadRecorder> m_recorder;

    std::unique_ptr<Session> makeSession()
    {
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.observer = m_recorder;
        o.transferTimeoutMs = 10000;
        o.retryBaseDelayMs = 10;
        return std::make_unique<Session>(o);
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_recorder = std::make_shared<ThreadRecorder>();
    }

    void cleanup()
    {
        delete m_server;
        m_server = nullptr;
    }

    void callbackRunsOnContextThreadRequestOnWorker()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto session = makeSession();
        QObject context;
        QThread* callbackThread = nullptr;
        std::optional<Result<void>> out;
        const RequestId id = session->headBucket(QStringLiteral("bkt"), &context, [&](Result<void> r) {
            callbackThread = QThread::currentThread();
            out.emplace(std::move(r));
        });
        QVERIFY(id != RequestId::Invalid);
        QVERIFY(!out.has_value());   // never synchronous
        QTRY_VERIFY(out.has_value());
        QVERIFY(*out);
        QCOMPARE(callbackThread, QThread::currentThread());

        QMutexLocker lock(&m_recorder->mutex);
        QCOMPARE(m_recorder->threads.size(), 1);
        QVERIFY(!m_recorder->threads.contains(QThread::currentThread()));   // network on the worker
    }

    void contextOnAnotherThread()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto session = makeSession();
        QThread other;
        other.start();
        QObject* context = new QObject;
        context->moveToThread(&other);
        std::atomic<QThread*> seen{nullptr};
        session->headBucket(QStringLiteral("bkt"), context,
                            [&](Result<void>) { seen = QThread::currentThread(); });
        QTRY_VERIFY(seen.load() != nullptr);
        QCOMPARE(seen.load(), &other);
        context->deleteLater();
        other.quit();
        other.wait();
    }

    void destroyedContextDropsCallback()
    {
        FakeResponse slow;
        slow.delayMs = 200;
        m_server->on("HEAD", QString(), slow);
        auto session = makeSession();
        auto* context = new QObject;
        bool called = false;
        session->headBucket(QStringLiteral("bkt"), context, [&](Result<void>) { called = true; });
        delete context;
        QTest::qWait(500);
        QVERIFY(!called);
        QCOMPARE(m_server->requestCount(), 1);
    }

    void nullContextIsRefused()
    {
        auto session = makeSession();
        QTest::ignoreMessage(QtWarningMsg, "request without a context object ignored");
        QCOMPARE(session->headBucket(QStringLiteral("bkt"), nullptr, [](Result<void>) {}),
                 RequestId::Invalid);
    }

    void cancelInFlight()
    {
        FakeResponse slow;
        slow.delayMs = 5000;
        m_server->on("HEAD", QString(), slow);
        auto session = makeSession();
        QObject context;
        std::optional<Result<void>> out;
        QElapsedTimer t;
        t.start();
        const RequestId id = session->headBucket(QStringLiteral("bkt"), &context,
                                                 [&](Result<void> r) { out.emplace(std::move(r)); });
        QTRY_COMPARE(m_server->requestCount(), 1);
        session->cancel(id);
        session->cancel(id);   // twice is harmless
        QTRY_VERIFY(out.has_value());
        QCOMPARE(out->error().category, ErrorCategory::Cancelled);
        QVERIFY(t.elapsed() < 3000);
        QTest::qWait(100);   // no second delivery
    }

    void cancelBeforeStartSendsNothing()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto session = makeSession();
        QObject context;
        int calls = 0;
        std::optional<ErrorCategory> category;
        const RequestId id = session->headBucket(QStringLiteral("bkt"), &context, [&](Result<void> r) {
            ++calls;
            category = r ? std::nullopt : std::optional(r.error().category);
        });
        session->cancel(id);
        QTRY_COMPARE(calls, 1);
        QCOMPARE(category, std::optional(ErrorCategory::Cancelled));
        QTest::qWait(200);
        QCOMPARE(calls, 1);
    }

    void destroyingSessionWithRequestsInFlight()
    {
        FakeResponse slow;
        slow.delayMs = 3000;
        m_server->on("", QString(), slow);
        QObject context;
        int calls = 0;
        QElapsedTimer t;
        t.start();
        {
            auto session = makeSession();
            for (int i = 0; i < 5; ++i)
                session->headBucket(QStringLiteral("b%1").arg(i), &context, [&](Result<void>) { ++calls; });
            QTRY_VERIFY(m_server->requestCount() >= 1);
        }
        QVERIFY(t.elapsed() < 2500);
        QTest::qWait(200);
        QCOMPARE(calls, 0);
    }

    void shutdownCancelsAndRefuses()
    {
        FakeResponse slow;
        slow.delayMs = 3000;
        m_server->on("", QString(), slow);
        auto session = makeSession();
        QSignalSpy finished(session.get(), &Session::shutdownFinished);
        QObject context;
        QList<ErrorCategory> results;
        for (int i = 0; i < 3; ++i) {
            session->headBucket(QStringLiteral("bkt"), &context,
                                [&](Result<void> r) { results.append(r.error().category); });
        }
        session->shutdown();
        QVERIFY(finished.wait(3000));
        QTRY_COMPARE(results.size(), 3);
        for (const ErrorCategory c : results)
            QCOMPARE(c, ErrorCategory::Cancelled);

        std::optional<ErrorCategory> late;
        session->headBucket(QStringLiteral("bkt"), &context,
                            [&](Result<void> r) { late = r.error().category; });
        QTRY_VERIFY(late.has_value());
        QCOMPARE(*late, ErrorCategory::Cancelled);
    }

    void stressFromManyThreads()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto session = makeSession();
        QObject context;
        constexpr int kThreads = 8;
        constexpr int kPerThread = 60;
        int ok = 0;
        int failed = 0;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    const RequestId id = session->headBucket(
                        QStringLiteral("b%1-%2").arg(t).arg(i), &context,
                        [&](Result<void> r) { r ? ++ok : ++failed; });   // context thread only
                    if (i % 7 == 3)
                        session->cancel(id);
                }
            });
        }
        for (std::thread& th : threads)
            th.join();
        QTRY_COMPARE_WITH_TIMEOUT(ok + failed, kThreads * kPerThread, 60000);
        QVERIFY(ok > 0);
        QTest::qWait(100);
        QCOMPARE(ok + failed, kThreads * kPerThread);   // exactly once each
    }

    void presignThroughSession()
    {
        m_server->on("GET", QString(), FakeResponse::ok("payload", "application/octet-stream"));
        auto session = makeSession();
        QObject context;
        std::optional<Result<QUrl>> url;
        PresignRequest req;
        req.bucket = QStringLiteral("bkt");
        req.key = QStringLiteral("a b.txt");
        session->presign(req, &context, [&](Result<QUrl> r) { url.emplace(std::move(r)); });
        QTRY_VERIFY(url.has_value());
        QVERIFY(*url);
        QNetworkAccessManager nam;
        QNetworkReply* reply = nam.get(QNetworkRequest(url->value()));
        QSignalSpy done(reply, &QNetworkReply::finished);
        QVERIFY(done.wait());
        QCOMPARE(reply->readAll(), QByteArray("payload"));
        delete reply;

        req.expiresSeconds = 8 * 24 * 3600;
        url.reset();
        session->presign(req, &context, [&](Result<QUrl> r) { url.emplace(std::move(r)); });
        QTRY_VERIFY(url.has_value());
        QCOMPARE(url->error().category, ErrorCategory::InvalidRequest);
    }

    void pagerWalksAllPages()
    {
        Matcher list;
        list.method = "GET";
        m_server->addRoute(list, {FakeResponse::ok(listPage(QStringLiteral("a"), QStringLiteral("t1")), kXml),
                                  FakeResponse::ok(listPage(QStringLiteral("b"), QStringLiteral("t2")), kXml),
                                  FakeResponse::ok(listPage(QStringLiteral("c"), QString()), kXml)});
        auto session = makeSession();
        QObject context;
        ListObjectsParams p;
        p.bucket = QStringLiteral("bkt");
        ListObjectsPager pager(*session, p);
        QStringList keys;
        while (pager.hasMore()) {
            bool got = false;
            QVERIFY(pager.fetchNext(&context, [&](Result<ListPage> r) {
                QVERIFY(r);
                for (const ObjectEntry& e : r->objects)
                    keys.append(e.key);
                got = true;
            }) != RequestId::Invalid);
            QVERIFY(pager.isFetching());
            QCOMPARE(pager.fetchNext(&context, [](Result<ListPage>) {}), RequestId::Invalid);
            QTRY_VERIFY(got);
        }
        QCOMPARE(keys, (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
        QCOMPARE(pager.pagesFetched(), 3);
        QCOMPARE(m_server->requests()[2].queryItem("continuation-token"), QByteArray("t2"));
    }

    void pagerDetectsLoopsAcrossPages()
    {
        // t1 → t2 → t1: each page differs from its own request, the walk loops.
        Matcher list;
        list.method = "GET";
        m_server->addRoute(list, {FakeResponse::ok(listPage(QStringLiteral("a"), QStringLiteral("t1")), kXml),
                                  FakeResponse::ok(listPage(QStringLiteral("b"), QStringLiteral("t2")), kXml),
                                  FakeResponse::ok(listPage(QStringLiteral("c"), QStringLiteral("t1")), kXml)});
        auto session = makeSession();
        QObject context;
        ListObjectsParams p;
        p.bucket = QStringLiteral("bkt");
        ListObjectsPager pager(*session, p);
        std::optional<Error> error;
        int pages = 0;
        while (pager.hasMore() && pages < 10) {
            bool got = false;
            pager.fetchNext(&context, [&](Result<ListPage> r) {
                if (!r)
                    error = r.error();
                got = true;
            });
            QTRY_VERIFY(got);
            ++pages;
        }
        QVERIFY(error.has_value());
        QCOMPARE(error->category, ErrorCategory::Protocol);
        QVERIFY(!pager.hasMore());
        QCOMPARE(pages, 3);
    }

    void clockSkewSignalCrossesThreads()
    {
        m_server->setClockOffsetSecs(30 * 60);
        m_server->on("HEAD", QString(), FakeResponse());
        auto session = makeSession();
        QSignalSpy skew(session.get(), &Session::clockSkewDetected);
        QObject context;
        bool ok = false;
        session->headBucket(QStringLiteral("bkt"), &context, [&](Result<void> r) { ok = bool(r); });
        QTRY_VERIFY(ok);
        QTRY_VERIFY(skew.size() >= 1);
    }
};

QTEST_GUILESS_MAIN(TestSession)
#include "test_session.moc"
