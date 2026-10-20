#include "client/S3Client.h"

#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QElapsedTimer>
#include <QMutex>
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

// Answers from a foreign thread, like a keychain would; counts calls.
class ThreadedProvider final : public CredentialsProvider {
public:
    explicit ThreadedProvider(std::optional<Credentials> creds) : m_creds(std::move(creds)) {}
    ~ThreadedProvider() override
    {
        for (std::thread& t : m_threads)
            t.join();
    }
    void fetch(Callback done) override
    {
        ++fetches;
        const std::optional<Credentials> creds = m_creds;
        QMutexLocker lock(&m_mutex);
        m_threads.emplace_back([done = std::move(done), creds] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (creds)
                done(*creds);
            else
                done(Error::make(ErrorCategory::Credentials, QStringLiteral("keychain locked")));
        });
    }
    void invalidate() override { ++invalidations; }

    std::atomic<int> fetches{0};
    std::atomic<int> invalidations{0};

private:
    std::optional<Credentials> m_creds;
    QMutex m_mutex;
    std::vector<std::thread> m_threads;
};

class RecordingObserver final : public RequestObserver {
public:
    void onRequestFinished(const RequestRecord& r) override
    {
        QMutexLocker lock(&mutex);
        records.append(r);
    }
    QMutex mutex;
    QList<RequestRecord> records;
};

template <typename T, typename Start>
Result<T> await(Start start)
{
    std::optional<Result<T>> out;
    start([&out](Result<T> r) { out.emplace(std::move(r)); });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, 15000))
        return Error::make(ErrorCategory::Internal, QStringLiteral("test timeout"));
    return std::move(*out);
}

const QByteArray kXml = "application/xml";

} // namespace

class TestS3ClientRead : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::shared_ptr<RecordingObserver> m_observer;

    std::unique_ptr<S3Client> makeClient(const std::shared_ptr<CredentialsProvider>& provider = {},
                                         int maxResponseBytes = 64 * 1024 * 1024)
    {
        ClientConfig cfg;
        cfg.endpoint.url = m_server->endpointUrl();
        cfg.endpoint.region = QStringLiteral("test-1");
        cfg.profile = std::make_shared<GenericS3Profile>();
        cfg.credentials = provider ? provider
                                   : std::make_shared<StaticCredentialsProvider>(testCredentials());
        cfg.observer = m_observer;
        cfg.maxResponseBytes = maxResponseBytes;
        cfg.transferTimeoutMs = 5000;
        return std::make_unique<S3Client>(cfg);
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_observer = std::make_shared<RecordingObserver>();
    }

    void cleanup()
    {
        delete m_server;
        m_server = nullptr;
    }

    void listBucketsFollowsPages()
    {
        const QByteArray page1 = "<ListAllMyBucketsResult><Owner><ID>me</ID></Owner><Buckets>"
                                 "<Bucket><Name>a</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                                 "</Buckets><ContinuationToken>t/1+</ContinuationToken></ListAllMyBucketsResult>";
        const QByteArray page2 = "<ListAllMyBucketsResult><Buckets>"
                                 "<Bucket><Name>b</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                                 "</Buckets></ListAllMyBucketsResult>";
        Matcher root;
        root.method = "GET";
        root.path = QRegularExpression(QStringLiteral("^/$"));
        m_server->addRoute(root, {FakeResponse::ok(page1, kXml), FakeResponse::ok(page2, kXml)});

        auto client = makeClient();
        const Result<BucketList> r =
            await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->buckets.size(), 2);
        QCOMPARE(r->buckets[1].name, QStringLiteral("b"));
        QCOMPARE(r->ownerId, QStringLiteral("me"));
        QCOMPARE(m_server->requests()[1].queryItem("continuation-token"), QByteArray("t%2F1%2B"));
    }

    void listBucketsDetectsLoops()
    {
        const QByteArray page = "<ListAllMyBucketsResult><Buckets/>"
                                "<ContinuationToken>same</ContinuationToken></ListAllMyBucketsResult>";
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(page, kXml));
        auto client = makeClient();
        const Result<BucketList> r =
            await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
        QCOMPARE(m_server->requestCount(), 2);
    }

    void listObjectsQueryAndDecoding()
    {
        const QByteArray body = "<ListBucketResult><Name>bkt</Name><Prefix>a%20b%2F</Prefix>"
                                "<IsTruncated>true</IsTruncated>"
                                "<Contents><Key>a%20b/c%2B.txt</Key><Size>3</Size><ETag>abc</ETag></Contents>"
                                "<NextContinuationToken>next</NextContinuationToken>"
                                "<EncodingType>url</EncodingType></ListBucketResult>";
        m_server->on("GET", QStringLiteral("^/bkt$"), FakeResponse::ok(body, kXml));
        auto client = makeClient();
        ListObjectsParams p;
        p.bucket = QStringLiteral("bkt");
        p.prefix = QStringLiteral("a b/");
        p.delimiter = QStringLiteral("/");
        p.maxKeys = 5000;   // clamped to the profile limit
        const Result<ListPage> r = await<ListPage>([&](auto cb) { client->listObjects(p, std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->objects.first().key, QStringLiteral("a b/c+.txt"));
        QCOMPARE(r->objects.first().etag, QStringLiteral("\"abc\""));   // normalised
        QCOMPARE(r->nextContinuationToken, QStringLiteral("next"));

        const RecordedRequest req = m_server->requests().first();
        QCOMPARE(req.queryItem("list-type"), QByteArray("2"));
        QCOMPARE(req.queryItem("encoding-type"), QByteArray("url"));
        QCOMPARE(req.queryItem("prefix"), QByteArray("a%20b%2F"));
        QCOMPARE(req.queryItem("delimiter"), QByteArray("%2F"));
        QCOMPARE(req.queryItem("max-keys"), QByteArray("1000"));
        QCOMPARE(req.header("Accept-Encoding"), QByteArray("identity"));
        QVERIFY(!req.header("User-Agent").isEmpty());
    }

    void listObjectsDetectsRepeatedToken()
    {
        const QByteArray body = "<ListBucketResult><IsTruncated>true</IsTruncated>"
                                "<NextContinuationToken>tok</NextContinuationToken></ListBucketResult>";
        m_server->on("GET", QStringLiteral("^/bkt$"), FakeResponse::ok(body, kXml));
        auto client = makeClient();
        ListObjectsParams p;
        p.bucket = QStringLiteral("bkt");
        p.continuationToken = QStringLiteral("tok");
        const Result<ListPage> r = await<ListPage>([&](auto cb) { client->listObjects(p, std::move(cb)); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
        QVERIFY(r.error().message.contains(QLatin1String("loop")));
    }

    void headObjectWithNastyKeys_data()
    {
        QTest::addColumn<QString>("key");
        QTest::newRow("space+plus") << QStringLiteral("dir/a b+c.txt");
        QTest::newRow("percent") << QStringLiteral("100%/x");
        QTest::newRow("sub-delims") << QStringLiteral("!$&'()*,;=:@");
        QTest::newRow("unicode") << QStringLiteral("äöü/文件/\U0001F600");
        QTest::newRow("dots in names") << QStringLiteral("a.b/.../.hidden/x.");
        QTest::newRow("double slash") << QStringLiteral("a//b");
        QTest::newRow("question+hash") << QStringLiteral("q?x#y");
    }

    void headObjectWithNastyKeys()
    {
        QFETCH(QString, key);
        FakeResponse head;
        head.withHeader("Content-Length", "1234")
            .withHeader("Last-Modified", "Tue, 20 Oct 2026 08:00:00 GMT")
            .withHeader("ETag", "\"d41d8cd98f00b204e9800998ecf8427e\"")
            .withHeader("Content-Type", "text/plain")
            .withHeader("x-amz-version-id", "v42")
            .withHeader("X-Amz-Meta-Owner", "rene");
        m_server->on("HEAD", QString(), head);

        auto client = makeClient();
        const Result<ObjectMeta> r = await<ObjectMeta>(
            [&](auto cb) { client->headObject(QStringLiteral("bkt"), key, QStringLiteral("v42"), std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->key, key);
        QCOMPARE(r->size, Q_INT64_C(1234));
        QCOMPARE(r->lastModified, QDateTime(QDate(2026, 10, 20), QTime(8, 0), QTimeZone::UTC));
        QCOMPARE(r->versionId, QStringLiteral("v42"));
        QCOMPARE(r->userMetadata.value(QStringLiteral("owner")), QStringLiteral("rene"));
        QCOMPARE(m_server->requests().first().queryItem("versionId"), QByteArray("v42"));
    }

    void dotSegmentKeysAreNeverSent()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto client = makeClient();
        const Result<ObjectMeta> r = await<ObjectMeta>([&](auto cb) {
            client->headObject(QStringLiteral("bkt"), QStringLiteral("a/../b"), {}, std::move(cb));
        });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void bucketLocation_data()
    {
        QTest::addColumn<QByteArray>("constraint");
        QTest::addColumn<QString>("region");
        QTest::newRow("us-east-1") << QByteArray() << QStringLiteral("us-east-1");
        QTest::newRow("EU") << QByteArray("EU") << QStringLiteral("eu-west-1");
        QTest::newRow("fsn1") << QByteArray("fsn1") << QStringLiteral("fsn1");
    }

    void bucketLocation()
    {
        QFETCH(QByteArray, constraint);
        QFETCH(QString, region);
        Matcher m;
        m.requiredQueryKeys = {"location"};
        m_server->addRoute(m, {FakeResponse::ok("<LocationConstraint>" + constraint + "</LocationConstraint>", kXml)});
        auto client = makeClient();
        const Result<QString> r = await<QString>(
            [&](auto cb) { client->getBucketLocation(QStringLiteral("bkt"), std::move(cb)); });
        QVERIFY(r);
        QCOMPARE(r.value(), region);
    }

    void errorsAreClassified()
    {
        m_server->on("GET", QStringLiteral("^/gone$"),
                     FakeResponse::s3Error(404, "NoSuchBucket", "The specified bucket does not exist")
                         .withHeader("x-amz-request-id", "RID-1"));
        FakeResponse forbidden;
        forbidden.status = 403;
        m_server->on("HEAD", QStringLiteral("^/locked$"), forbidden);
        FakeResponse missing;
        missing.status = 404;
        m_server->on("HEAD", QStringLiteral("^/bkt/none$"), missing);
        FakeResponse drop;
        drop.dropWithoutResponse = true;
        m_server->on("HEAD", QStringLiteral("^/flaky$"), drop);
        auto client = makeClient();

        ListObjectsParams p;
        p.bucket = QStringLiteral("gone");
        Result<ListPage> list = await<ListPage>([&](auto cb) { client->listObjects(p, std::move(cb)); });
        QCOMPARE(list.error().category, ErrorCategory::NotFound);
        QCOMPARE(list.error().code, QStringLiteral("NoSuchBucket"));
        QCOMPARE(list.error().httpStatus, 404);
        QCOMPARE(list.error().resource, QStringLiteral("gone"));

        Result<void> head = await<void>([&](auto cb) { client->headBucket(QStringLiteral("locked"), std::move(cb)); });
        QCOMPARE(head.error().category, ErrorCategory::NotPermitted);

        Result<ObjectMeta> obj = await<ObjectMeta>(
            [&](auto cb) { client->headObject(QStringLiteral("bkt"), QStringLiteral("none"), {}, std::move(cb)); });
        QCOMPARE(obj.error().category, ErrorCategory::NotFound);

        head = await<void>([&](auto cb) { client->headBucket(QStringLiteral("flaky"), std::move(cb)); });
        QCOMPARE(head.error().category, ErrorCategory::Transient);
        QVERIFY(!head.error().outcomeUnknown);   // HEAD is idempotent
    }

    void abortCancelsQuickly()
    {
        FakeResponse slow = FakeResponse::ok("<ListAllMyBucketsResult/>", kXml);
        slow.delayMs = 3000;
        m_server->on("GET", QString(), slow);
        auto client = makeClient();

        std::optional<Result<BucketList>> out;
        QElapsedTimer t;
        t.start();
        const quint64 id = client->listBuckets([&](Result<BucketList> r) { out.emplace(std::move(r)); });
        QTRY_COMPARE(m_server->requestCount(), 1);
        client->abort(id);
        QTRY_VERIFY(out.has_value());
        QCOMPARE(out->error().category, ErrorCategory::Cancelled);
        QVERIFY(t.elapsed() < 2500);
    }

    void credentialsFromForeignThread()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto provider = std::make_shared<ThreadedProvider>(testCredentials());
        auto client = makeClient(provider);
        for (int i = 0; i < 3; ++i) {
            const Result<void> r = await<void>(
                [&](auto cb) { client->headBucket(QStringLiteral("bkt"), std::move(cb)); });
            QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        }
        QCOMPARE(provider->fetches.load(), 1);   // cached afterwards
    }

    void credentialFailureSendsNothing()
    {
        auto provider = std::make_shared<ThreadedProvider>(std::nullopt);
        auto client = makeClient(provider);
        const Result<void> r =
            await<void>([&](auto cb) { client->headBucket(QStringLiteral("bkt"), std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Credentials);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void rejectedCredentialsAreInvalidated()
    {
        Credentials wrong = testCredentials();
        wrong.secretAccessKey = QStringLiteral("nope");
        auto provider = std::make_shared<ThreadedProvider>(wrong);
        auto client = makeClient(provider);
        const Result<BucketList> r =
            await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Authentication);
        QCOMPARE(r.error().code, QStringLiteral("SignatureDoesNotMatch"));
        QCOMPARE(provider->invalidations.load(), 1);

        (void)await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QCOMPARE(provider->fetches.load(), 2);   // reloaded after the rejection
    }

    void oversizedResponseIsRefused()
    {
        m_server->on("GET", QString(), FakeResponse::ok(QByteArray(200000, 'x'), kXml));
        auto client = makeClient({}, 1000);
        const Result<BucketList> r =
            await<BucketList>([&](auto cb) { client->listBuckets(std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
    }

    void observerSeesRedactedRecords()
    {
        m_server->on("HEAD", QString(), FakeResponse());
        auto client = makeClient();
        QVERIFY(await<void>([&](auto cb) { client->headBucket(QStringLiteral("bkt"), std::move(cb)); }));
        QMutexLocker lock(&m_observer->mutex);
        QCOMPARE(m_observer->records.size(), 1);
        const RequestRecord& rec = m_observer->records.first();
        QCOMPARE(rec.operation, QStringLiteral("HeadBucket"));
        QCOMPARE(rec.method, QByteArray("HEAD"));
        QCOMPARE(rec.bucket, QStringLiteral("bkt"));
        QCOMPARE(rec.httpStatus, 200);
        QVERIFY(rec.ok);
        QVERIFY(!rec.mutating);
        QVERIFY(rec.requestId.startsWith(QLatin1String("FAKE")));
        QVERIFY(!rec.url.contains(QLatin1String("Signature")));
    }

    void callbacksAreNeverSynchronous()
    {
        auto client = makeClient();
        bool called = false;
        client->headBucket(QString(), [&](const Result<void>&) { called = true; });
        QVERIFY(!called);   // validation error, still delivered later
        QTRY_VERIFY(called);
    }
};

QTEST_GUILESS_MAIN(TestS3ClientRead)
#include "test_s3client_read.moc"
