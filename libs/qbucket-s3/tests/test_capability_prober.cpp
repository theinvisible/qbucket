#include "client/S3Client.h"

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QSignalSpy>
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

template <typename T, typename Start>
Result<T> await(Start start)
{
    std::optional<Result<T>> out;
    start([&out](Result<T> r) { out.emplace(std::move(r)); });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, 30000))
        return Error::make(ErrorCategory::Internal, QStringLiteral("test timeout"));
    return std::move(*out);
}

const QByteArray kXml = "application/xml";

void route(FakeS3Server& s, const QByteArray& subresource, const FakeResponse& r, bool objectLevel = false)
{
    Matcher m;
    m.method = "GET";
    m.requiredQueryKeys = {subresource};
    m.path = QRegularExpression(objectLevel ? QStringLiteral("^/bkt/.+") : QStringLiteral("^/bkt$"));
    s.addRoute(m, {r});
}

} // namespace

class TestCapabilityProber : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;

    ClientConfig config()
    {
        ClientConfig cfg;
        cfg.endpoint.url = m_server->endpointUrl();
        cfg.endpoint.region = QStringLiteral("test-1");
        cfg.profile = std::make_shared<GenericS3Profile>();
        cfg.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        cfg.retry.baseDelayMs = 1;
        cfg.retry.maxDelayMs = 5;
        cfg.transferTimeoutMs = 5000;
        return cfg;
    }

    // A server that knows a few subresources, ignores one, refuses others.
    void scriptServer(bool withObject)
    {
        FakeResponse listing = FakeResponse::ok(
            withObject ? QByteArray("<ListBucketResult><Name>bkt</Name><IsTruncated>false</IsTruncated>"
                                    "<Contents><Key>some%20file</Key></Contents><EncodingType>url</EncodingType></ListBucketResult>")
                       : QByteArray("<ListBucketResult><Name>bkt</Name><IsTruncated>false</IsTruncated></ListBucketResult>"),
            kXml);
        listing.withHeader("Server", "FakeRGW/1.0");
        route(*m_server, "versioning", FakeResponse::ok("<VersioningConfiguration><Status>Enabled</Status></VersioningConfiguration>", kXml));
        route(*m_server, "object-lock", FakeResponse::s3Error(404, "ObjectLockConfigurationNotFoundError"));
        route(*m_server, "lifecycle", FakeResponse::s3Error(501, "NotImplemented"));
        route(*m_server, "cors", FakeResponse::s3Error(503, "SlowDown"));
        route(*m_server, "acl", FakeResponse::s3Error(403, "AccessDenied"));
        // Ignores ?ownershipControls and lists the bucket instead.
        route(*m_server, "ownershipControls", listing);
        route(*m_server, "tagging", FakeResponse::ok("<Tagging><TagSet/></Tagging>", kXml), true);
        route(*m_server, "retention", FakeResponse::s3Error(404, "NoSuchObjectLockConfiguration"), true);
        Matcher list;
        list.method = "GET";
        list.requiredQueryKeys = {"list-type"};
        m_server->addRoute(list, {listing});
        // Everything else: not implemented.
        m_server->setFallbackHandler([](const RecordedRequest&) {
            return FakeResponse::s3Error(501, "NotImplemented");
        });
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
    }

    void cleanup()
    {
        delete m_server;
        m_server = nullptr;
    }

    void probesABucket()
    {
        scriptServer(true);
        S3Client client(config());
        const Result<CapabilitySet> r = await<CapabilitySet>([&](auto cb) { client.probeBucket(QStringLiteral("bkt"), {}, std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        const CapabilitySet& set = r.value();
        QCOMPARE(set.scope, CapabilityScope::Bucket);
        QCOMPARE(set.bucket, QStringLiteral("bkt"));
        QCOMPARE(set.state(Feature::BucketVersioning), CapabilityState::Supported);
        QCOMPARE(set.bucketState.versioning, QStringLiteral("Enabled"));
        QCOMPARE(set.state(Feature::ObjectLock), CapabilityState::Supported);
        QCOMPARE(set.bucketState.objectLockEnabled, std::optional(false));
        QCOMPARE(set.state(Feature::Lifecycle), CapabilityState::Unsupported);
        QCOMPARE(set.state(Feature::Cors), CapabilityState::Unknown);            // 503 after retries
        QCOMPARE(set.state(Feature::BucketAcl), CapabilityState::NotPermitted);
        QCOMPARE(set.state(Feature::OwnershipControls), CapabilityState::Unsupported);   // ignored
        QCOMPARE(set.state(Feature::ObjectTagging), CapabilityState::Supported);
        QCOMPARE(set.state(Feature::Retention), CapabilityState::Supported);
        QCOMPARE(set.state(Feature::Website), CapabilityState::Unsupported);
        QCOMPARE(set.fingerprint.serverHeader, QStringLiteral("FakeRGW/1.0"));
        // Write-only features are not probed.
        QVERIFY(!set.contains(Feature::CrossBucketCopy));
        QVERIFY(!set.contains(Feature::ConditionalWrites));

        // Object probes went to the listed key, and nothing but GETs was sent.
        bool objectProbe = false;
        for (const RecordedRequest& req : m_server->requests()) {
            QCOMPARE(req.method, QByteArray("GET"));
            objectProbe = objectProbe || req.path == "/bkt/some%20file";
        }
        QVERIFY(objectProbe);
    }

    void emptyBucketLeavesObjectFeaturesUnknown()
    {
        scriptServer(false);
        S3Client client(config());
        const Result<CapabilitySet> r = await<CapabilitySet>([&](auto cb) { client.probeBucket(QStringLiteral("bkt"), {}, std::move(cb)); });
        QVERIFY(r);
        QCOMPARE(r->state(Feature::ObjectTagging), CapabilityState::Unknown);
        QCOMPARE(r->entry(Feature::ObjectTagging).s3Code, QStringLiteral("NoObjectToProbe"));
    }

    void missingBucketIsAnError()
    {
        m_server->setFallbackHandler([](const RecordedRequest&) {
            return FakeResponse::s3Error(404, "NoSuchBucket");
        });
        S3Client client(config());
        const Result<CapabilitySet> r = await<CapabilitySet>([&](auto cb) { client.probeBucket(QStringLiteral("bkt"), {}, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::NotFound);
        QCOMPARE(m_server->requestCount(), 1);
    }

    void probeServerFingerprints()
    {
        m_server->on("GET", QStringLiteral("^/$"),
                     FakeResponse::ok("<ListAllMyBucketsResult><Buckets/></ListAllMyBucketsResult>", kXml)
                         .withHeader("Server", "FakeRGW/1.0"));
        S3Client client(config());
        const Result<CapabilitySet> r = await<CapabilitySet>([&](auto cb) { client.probeServer(std::move(cb)); });
        QVERIFY(r);
        QCOMPARE(r->scope, CapabilityScope::Server);
        QCOMPARE(r->fingerprint.serverHeader, QStringLiteral("FakeRGW/1.0"));
        QCOMPARE(r->state(Feature::Presign), CapabilityState::Supported);   // profile knowledge
    }

    void unsupportedFeaturesAreRefusedLocally()
    {
        m_server->on("POST", QString(), FakeResponse::ok("<DeleteResult/>", kXml));
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        Session session(o);
        QObject ctx;

        CapabilitySet set;
        set.scope = CapabilityScope::Bucket;
        set.bucket = QStringLiteral("bkt");
        set.set(Feature::BatchDelete, {CapabilityState::Unsupported, CapabilitySource::Probed,
                                       QDateTime::currentDateTimeUtc(), 501, {}, false, 0});
        session.setCapabilities(set);

        Result<DeleteObjectsResult> r = await<DeleteObjectsResult>([&](auto cb) {
            session.deleteObjects(QStringLiteral("bkt"), {{QStringLiteral("k"), {}}}, &ctx, std::move(cb));
        });
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QCOMPARE(r.error().code, QStringLiteral("CapabilityUnsupported"));
        QCOMPARE(m_server->requestCount(), 0);   // never sent

        // Another bucket is not affected.
        r = await<DeleteObjectsResult>([&](auto cb) {
            session.deleteObjects(QStringLiteral("other"), {{QStringLiteral("k"), {}}}, &ctx, std::move(cb));
        });
        QVERIFY(r);
        QCOMPARE(m_server->requestCount(), 1);
    }

    void unsupportedIsLearnedFromRealCalls()
    {
        m_server->on("POST", QString(), FakeResponse::s3Error(501, "NotImplemented"));
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        Session session(o);
        QSignalSpy learned(&session, &Session::capabilityLearned);
        QObject ctx;

        Result<DeleteObjectsResult> r = await<DeleteObjectsResult>([&](auto cb) {
            session.deleteObjects(QStringLiteral("bkt"), {{QStringLiteral("k"), {}}}, &ctx, std::move(cb));
        });
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QTRY_COMPARE(learned.size(), 1);
        QCOMPARE(learned.first().at(0).toString(), QStringLiteral("bkt"));
        QCOMPARE(learned.first().at(1).toString(), QStringLiteral("bucket.batch-delete"));

        r = await<DeleteObjectsResult>([&](auto cb) {
            session.deleteObjects(QStringLiteral("bkt"), {{QStringLiteral("k"), {}}}, &ctx, std::move(cb));
        });
        QCOMPARE(r.error().code, QStringLiteral("CapabilityUnsupported"));
        QCOMPARE(m_server->requestCount(), 1);   // the second one never left
    }
};

QTEST_GUILESS_MAIN(TestCapabilityProber)
#include "test_capability_prober.moc"
