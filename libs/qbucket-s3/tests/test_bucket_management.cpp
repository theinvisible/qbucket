// Bucket configuration, object lock and buckets themselves through the
// Session against the FakeObjectStore: reading (nothing set, set, ignored
// subresource), writing and deleting with Content-MD5, the guards against a
// proxy that runs PUT ?x as CreateBucket and DELETE ?x as DeleteBucket,
// retention / legal hold with the governance bypass, default retention,
// CreateBucket / DeleteBucket, the capability short-circuit.

#include <qbucket/s3/BucketConfig.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QFile>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

const QString kBucket = QStringLiteral("bkt");

Credentials testCredentials()
{
    Credentials c;
    c.accessKeyId = QStringLiteral("AKIDTEST");
    c.secretAccessKey = QStringLiteral("test/secret+key");
    return c;
}

QByteArray fixture(const char* name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/bucket-config/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", name);
    return f.readAll();
}

} // namespace

class TestBucketManagement : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    std::unique_ptr<Session> m_session;
    QObject m_context;

    template <typename T, typename Call>
    Result<T> call(Call&& start)
    {
        std::optional<Result<T>> out;
        start([&out](Result<T> r) { out.emplace(std::move(r)); });
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 10000))
            return Error::make(ErrorCategory::Internal, QStringLiteral("no answer"));
        return std::move(*out);
    }

    Result<BucketConfigDocument> get(BucketConfig kind, const QString& bucket = kBucket)
    {
        return call<BucketConfigDocument>(
            [&](auto cb) { m_session->getBucketConfig(bucket, kind, &m_context, cb); });
    }

    Result<SubresourceWriteResult> put(BucketConfig kind, const QByteArray& body, const QString& canned = {})
    {
        PutBucketConfigParams p;
        p.bucket = kBucket;
        p.kind = kind;
        p.body = body;
        p.cannedAcl = canned;
        return call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketConfig(p, &m_context, cb); });
    }

    Result<SubresourceWriteResult> remove(BucketConfig kind)
    {
        return call<SubresourceWriteResult>(
            [&](auto cb) { m_session->deleteBucketConfig(kBucket, kind, &m_context, cb); });
    }

    Result<DeleteObjectResult> deleteVersion(const QString& key, const QString& versionId, bool bypass)
    {
        DeleteObjectParams p{kBucket, key, versionId, bypass};
        return call<DeleteObjectResult>([&](auto cb) { m_session->deleteObject(p, &m_context, cb); });
    }

    Result<SubresourceWriteResult> setRetention(const QString& key, const QString& versionId,
                                                std::optional<ObjectLockMode> mode, const QDateTime& until,
                                                bool bypass = false)
    {
        PutObjectRetentionParams p;
        p.bucket = kBucket;
        p.key = key;
        p.versionId = versionId;
        p.mode = mode;
        p.retainUntil = until;
        p.bypassGovernanceRetention = bypass;
        return call<SubresourceWriteResult>([&](auto cb) { m_session->putObjectRetention(p, &m_context, cb); });
    }

    Result<ObjectRetention> retention(const QString& key, const QString& versionId)
    {
        return call<ObjectRetention>(
            [&](auto cb) { m_session->getObjectRetention(kBucket, key, versionId, &m_context, cb); });
    }

    Result<SubresourceWriteResult> setLegalHold(const QString& key, const QString& versionId, bool on)
    {
        PutObjectLegalHoldParams p{kBucket, key, versionId, on};
        return call<SubresourceWriteResult>([&](auto cb) { m_session->putObjectLegalHold(p, &m_context, cb); });
    }

    bool sentMd5(const QByteArray& query)
    {
        for (const RecordedRequest& r : m_server->requests()) {
            if (r.method == "PUT" && r.query.startsWith(query))
                return !r.header("content-md5").isEmpty();
        }
        return false;
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
        m_store->putObject(kBucket, QStringLiteral("guard"), "do not delete");
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.retryBaseDelayMs = 5;
        m_session = std::make_unique<Session>(o);
    }

    void cleanup()
    {
        m_session.reset();
        m_store.reset();
        delete m_server;
        m_server = nullptr;
    }

    void nothingSetIsNotAnError()
    {
        for (const BucketConfig kind : {BucketConfig::Policy, BucketConfig::PolicyStatus, BucketConfig::Cors,
                                        BucketConfig::Lifecycle, BucketConfig::ObjectLock,
                                        BucketConfig::PublicAccessBlock, BucketConfig::Tagging,
                                        BucketConfig::Website, BucketConfig::Logging, BucketConfig::Replication,
                                        BucketConfig::Encryption, BucketConfig::Notifications,
                                        BucketConfig::OwnershipControls}) {
            const Result<BucketConfigDocument> doc = get(kind);
            QVERIFY2(doc, qPrintable(bucketConfigName(kind) + QLatin1Char(' ')
                                     + (doc ? QString() : doc.error().toString())));
            QCOMPARE(doc->kind, kind);
            QVERIFY2(!doc->configured, qPrintable(bucketConfigName(kind)));
        }
        const Result<BucketConfigDocument> acl = get(BucketConfig::Acl);
        QVERIFY(acl);
        QVERIFY(acl->configured);   // a bucket always has an ACL
        QCOMPARE(cannedAclOf(*parseAccessControlPolicyDocument(acl->body)), std::optional(QStringLiteral("private")));
        // A missing bucket stays an error.
        const Result<BucketConfigDocument> missing = get(BucketConfig::Cors, QStringLiteral("nope"));
        QVERIFY(!missing);
        QCOMPARE(missing.error().category, ErrorCategory::NotFound);
    }

    void writeReadAndDelete_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QByteArray>("document");
        QTest::addColumn<QByteArray>("query");
        QTest::newRow("policy") << int(BucketConfig::Policy) << fixture("policy-public-read.json") << QByteArray("policy");
        QTest::newRow("cors") << int(BucketConfig::Cors) << fixture("cors.xml") << QByteArray("cors");
        QTest::newRow("lifecycle") << int(BucketConfig::Lifecycle) << fixture("lifecycle.xml") << QByteArray("lifecycle");
        QTest::newRow("public access block") << int(BucketConfig::PublicAccessBlock)
                                             << fixture("public-access-block.xml") << QByteArray("publicAccessBlock");
    }

    void writeReadAndDelete()
    {
        QFETCH(int, kind);
        QFETCH(QByteArray, document);
        QFETCH(QByteArray, query);
        const auto k = BucketConfig(kind);
        const Result<SubresourceWriteResult> w = put(k, document);
        QVERIFY2(w, qPrintable(w ? QString() : w.error().toString()));
        QVERIFY(sentMd5(query));
        const Result<BucketConfigDocument> doc = get(k);
        QVERIFY(doc);
        QVERIFY(doc->configured);
        QVERIFY(equivalentBucketConfig(k, document, doc->body));
        QVERIFY(remove(k));
        QVERIFY(!get(k)->configured);
        // Deleting what isn't set succeeds (also a retry after a lost answer).
        QVERIFY(remove(k));
        QVERIFY(m_store->hasBucket(kBucket));
    }

    void policyStatusFollowsThePolicy()
    {
        QVERIFY(put(BucketConfig::Policy, fixture("policy-public-read.json")));
        const Result<BucketConfigDocument> status = get(BucketConfig::PolicyStatus);
        QVERIFY(status && status->configured);
        const Result<bool> isPublic = parsePolicyStatus(status->body);
        QVERIFY(isPublic && *isPublic);
    }

    void bucketAcl()
    {
        QVERIFY(put(BucketConfig::Acl, {}, QStringLiteral("public-read")));
        QCOMPARE(cannedAclOf(*parseAccessControlPolicyDocument(get(BucketConfig::Acl)->body)),
                 std::optional(QStringLiteral("public-read")));
        // Either a canned ACL or a document, not both, not none.
        QCOMPARE(put(BucketConfig::Acl, {}).error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(put(BucketConfig::Cors, fixture("cors.xml"), QStringLiteral("private")).error().category,
                 ErrorCategory::InvalidRequest);
    }

    void onlyDocumentsOfTheKindAreSent()
    {
        const int before = m_server->requests().size();
        QCOMPARE(put(BucketConfig::Cors, fixture("lifecycle.xml")).error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(put(BucketConfig::Policy, "<xml/>").error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(put(BucketConfig::Website, "<WebsiteConfiguration/>").error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(remove(BucketConfig::ObjectLock).error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requests().size(), before);
    }

    void theServerRefusesABadDocument()
    {
        QByteArray bad = fixture("cors.xml");
        bad.replace("<AllowedMethod>PUT</AllowedMethod>", "<AllowedMethod>PATCH</AllowedMethod>");
        const Result<SubresourceWriteResult> w = put(BucketConfig::Cors, bad);
        QVERIFY(!w);
        QCOMPARE(w.error().code, QStringLiteral("MalformedXML"));
    }

    void anIgnoredSubresourceIsUnsupported()
    {
        // A server that ignores ?cors answers the bucket listing.
        m_server->on("GET", QStringLiteral("^/bkt/?$"),
                     FakeResponse::ok("<ListBucketResult><Name>bkt</Name></ListBucketResult>", "application/xml"));
        const Result<BucketConfigDocument> doc = get(BucketConfig::Cors);
        QVERIFY(!doc);
        QCOMPARE(doc.error().category, ErrorCategory::Unsupported);
        QCOMPARE(doc.error().code, QStringLiteral("Root:ListBucketResult"));
    }

    void writesThroughAProxyThatDropsTheSubresource()
    {
        m_store->setInterceptSubresourceWrites(true);
        // PUT ?cors ran as CreateBucket on an existing bucket: 409 → Protocol.
        Result<SubresourceWriteResult> w = put(BucketConfig::Cors, fixture("cors.xml"));
        QVERIFY(!w);
        QCOMPARE(w.error().category, ErrorCategory::Protocol);
        QVERIFY(w.error().message.contains(QLatin1String("CreateBucket")));
        // DELETE ?cors ran as DeleteBucket on a bucket with an object: 409 → Protocol.
        w = remove(BucketConfig::Cors);
        QVERIFY(!w);
        QCOMPARE(w.error().category, ErrorCategory::Protocol);
        QVERIFY(w.error().message.contains(QLatin1String("DeleteBucket")));
        QVERIFY(m_store->hasBucket(kBucket));

        // On a missing bucket the PUT creates one — the Location header gives it away.
        PutBucketConfigParams p;
        p.bucket = QStringLiteral("created-by-accident");
        p.kind = BucketConfig::Cors;
        p.body = fixture("cors.xml");
        w = call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketConfig(p, &m_context, cb); });
        QVERIFY(!w);
        QCOMPARE(w.error().category, ErrorCategory::Protocol);
        QVERIFY(m_store->hasBucket(QStringLiteral("created-by-accident")));
    }

    void createAndDeleteBuckets()
    {
        CreateBucketParams p;
        p.bucket = QStringLiteral("new-bucket");
        p.objectLockEnabled = true;
        QVERIFY(call<void>([&](auto cb) { m_session->createBucket(p, &m_context, cb); }));
        QVERIFY(m_store->hasBucket(p.bucket));
        QCOMPARE(m_store->versioning(p.bucket), QStringLiteral("Enabled"));
        const Result<BucketConfigDocument> lock = get(BucketConfig::ObjectLock, p.bucket);
        QVERIFY(lock && lock->configured);
        QVERIFY(parseObjectLockConfiguration(lock->body)->enabled);
        const Result<void> again = call<void>([&](auto cb) { m_session->createBucket(p, &m_context, cb); });
        QCOMPARE(again.error().category, ErrorCategory::Conflict);
        QCOMPARE(again.error().code, QStringLiteral("BucketAlreadyOwnedByYou"));

        // Not empty while any version (or upload) is left.
        m_store->putObject(p.bucket, QStringLiteral("a"), "x");
        m_store->deleteObject(p.bucket, QStringLiteral("a"));
        Result<void> del = call<void>([&](auto cb) { m_session->deleteBucket(p.bucket, &m_context, cb); });
        QCOMPARE(del.error().code, QStringLiteral("BucketNotEmpty"));
        for (const FakeObjectStore::Version& v : m_store->versions(p.bucket, QStringLiteral("a"))) {
            QVERIFY(call<DeleteObjectResult>(
                [&](auto cb) { m_session->deleteObject(p.bucket, QStringLiteral("a"), v.versionId, &m_context, cb); }));
        }
        del = call<void>([&](auto cb) { m_session->deleteBucket(p.bucket, &m_context, cb); });
        QVERIFY(del);
        QVERIFY(!m_store->hasBucket(p.bucket));
    }

    void defaultRetentionAndTheBypass()
    {
        m_store->setObjectLockEnabled(kBucket);
        ObjectLockConfiguration config;
        config.enabled = true;
        config.mode = ObjectLockMode::Governance;
        config.days = 1;
        QVERIFY(put(BucketConfig::ObjectLock, buildObjectLockConfiguration(config)));
        QVERIFY(sentMd5("object-lock"));
        const QString v1 = m_store->putObject(kBucket, QStringLiteral("doc"), "one");   // seeded: no default
        // A PUT over HTTP gets the default retention.
        PutObjectParams up;
        up.bucket = kBucket;
        up.key = QStringLiteral("doc");
        up.data = "two";
        const Result<PutObjectResult> put2 = call<PutObjectResult>([&](auto cb) { m_session->putObject(up, &m_context, cb); });
        QVERIFY(put2);
        const QString v2 = put2->versionId;
        const Result<ObjectRetention> r2 = retention(QStringLiteral("doc"), v2);
        QVERIFY(r2);
        QCOMPARE(r2->mode, std::optional(ObjectLockMode::Governance));
        QVERIFY(r2->retainUntil > QDateTime::currentDateTimeUtc().addSecs(qint64(23) * 3600));
        const Result<ObjectRetention> r1 = retention(QStringLiteral("doc"), v1);
        QVERIFY(r1);
        QVERIFY(!r1->mode);

        // GOVERNANCE: deleting needs the bypass, shortening too.
        Result<DeleteObjectResult> del = deleteVersion(QStringLiteral("doc"), v2, false);
        QCOMPARE(del.error().category, ErrorCategory::NotPermitted);
        QCOMPARE(setRetention(QStringLiteral("doc"), v2, ObjectLockMode::Governance,
                              QDateTime::currentDateTimeUtc().addSecs(60))
                     .error().category,
                 ErrorCategory::NotPermitted);
        QVERIFY(setRetention(QStringLiteral("doc"), v2, ObjectLockMode::Governance,
                             QDateTime::currentDateTimeUtc().addDays(2)));   // extending is fine
        del = deleteVersion(QStringLiteral("doc"), v2, true);
        QVERIFY2(del, qPrintable(del ? QString() : del.error().toString()));
        QCOMPARE(m_server->requests().last().header("x-amz-bypass-governance-retention"), QByteArray("true"));

        // Removing a retention is a bypass, never implicit.
        m_store->setRetention(kBucket, QStringLiteral("doc"), v1, ObjectLockMode::Governance,
                              QDateTime::currentDateTimeUtc().addDays(1));
        QCOMPARE(setRetention(QStringLiteral("doc"), v1, std::nullopt, {}).error().category,
                 ErrorCategory::InvalidRequest);
        QVERIFY(setRetention(QStringLiteral("doc"), v1, std::nullopt, {}, true));
        QVERIFY(!retention(QStringLiteral("doc"), v1)->mode);
    }

    void complianceAndLegalHoldHoldEvenWithTheBypass()
    {
        m_store->setObjectLockEnabled(kBucket);
        const QString v = m_store->putObject(kBucket, QStringLiteral("legal"), "x");
        m_store->setRetention(kBucket, QStringLiteral("legal"), v, ObjectLockMode::Compliance,
                              QDateTime::currentDateTimeUtc().addDays(1));
        QCOMPARE(deleteVersion(QStringLiteral("legal"), v, true).error().category, ErrorCategory::NotPermitted);
        QCOMPARE(setRetention(QStringLiteral("legal"), v, std::nullopt, {}, true).error().category,
                 ErrorCategory::NotPermitted);
        QCOMPARE(setRetention(QStringLiteral("legal"), v, ObjectLockMode::Governance,
                              QDateTime::currentDateTimeUtc().addDays(5), true)
                     .error().category,
                 ErrorCategory::NotPermitted);   // COMPLIANCE stays COMPLIANCE

        const QString h = m_store->putObject(kBucket, QStringLiteral("held"), "x");
        QVERIFY(setLegalHold(QStringLiteral("held"), h, true));
        QVERIFY(sentMd5("legal-hold"));
        const Result<bool> held = call<bool>(
            [&](auto cb) { m_session->getObjectLegalHold(kBucket, QStringLiteral("held"), h, &m_context, cb); });
        QVERIFY(held && *held);
        // A batch with the bypass still refuses the held version.
        DeleteObjectsParams batch;
        batch.bucket = kBucket;
        batch.objects = {{QStringLiteral("held"), h}};
        batch.bypassGovernanceRetention = true;
        const Result<DeleteObjectsResult> r =
            call<DeleteObjectsResult>([&](auto cb) { m_session->deleteObjects(batch, &m_context, cb); });
        QVERIFY(r);
        QCOMPARE(r->failed.size(), 1);
        QCOMPARE(r->failed[0].code, QStringLiteral("AccessDenied"));
        QVERIFY(setLegalHold(QStringLiteral("held"), h, false));
        QVERIFY(deleteVersion(QStringLiteral("held"), h, false));
    }

    void retentionOnABucketWithoutObjectLock()
    {
        const Result<ObjectRetention> r = retention(QStringLiteral("guard"), {});
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
    }

    void unsupportedKindsAreRefusedLocallyOnceKnown()
    {
        m_store->setUnsupportedBucketConfig(BucketConfig::Cors);
        const Result<BucketConfigDocument> doc = get(BucketConfig::Cors);
        QCOMPARE(doc.error().category, ErrorCategory::Unsupported);
        // Learned: the next write isn't sent at all.
        CapabilitySet set;
        set.scope = CapabilityScope::Bucket;
        set.bucket = kBucket;
        set.set(Feature::CorsWrite, {CapabilityState::Unsupported, CapabilitySource::Profile,
                                     QDateTime::currentDateTimeUtc(), 0, {}, false, 0});
        m_session->setCapabilities(set);
        const int before = m_store->count(QStringLiteral("PutBucketCors"));
        const Result<SubresourceWriteResult> w = put(BucketConfig::Cors, fixture("cors.xml"));
        QCOMPARE(w.error().code, QStringLiteral("CapabilityUnsupported"));
        QCOMPARE(m_store->count(QStringLiteral("PutBucketCors")), before);
    }
};

QTEST_GUILESS_MAIN(TestBucketManagement)
#include "test_bucket_management.moc"
