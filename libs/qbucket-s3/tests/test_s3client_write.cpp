#include "client/S3Client.h"

#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <QFile>
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
    if (!QTest::qWaitFor([&] { return out.has_value(); }, 15000))
        return Error::make(ErrorCategory::Internal, QStringLiteral("test timeout"));
    return std::move(*out);
}

QByteArray md5Base64(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toBase64();
}

QByteArray md5Hex(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

QStringList keysIn(const QByteArray& deleteBody)
{
    QXmlStreamReader r(deleteBody);
    QStringList keys;
    while (!r.atEnd()) {
        if (r.readNextStartElement() && r.name() == QLatin1String("Key"))
            keys.append(r.readElementText());
    }
    return keys;
}

const QByteArray kXml = "application/xml";

} // namespace

class TestS3ClientWrite : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;

    std::unique_ptr<S3Client> makeClient()
    {
        ClientConfig cfg;
        cfg.endpoint.url = m_server->endpointUrl();
        cfg.endpoint.region = QStringLiteral("test-1");
        cfg.profile = std::make_shared<GenericS3Profile>();
        cfg.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        cfg.transferTimeoutMs = 5000;
        return std::make_unique<S3Client>(cfg);
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

    void putObjectSendsIntegrityHeaders()
    {
        m_server->on("PUT", QString(), FakeResponse().withHeader("ETag", "\"e1\"").withHeader("x-amz-version-id", "v7"));
        auto client = makeClient();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("dir/file name.bin");
        p.data = QByteArray(100000, '\x5a') + "tail";
        p.metadata.insert(QStringLiteral("Owner"), QStringLiteral("rene"));
        p.storageClass = QStringLiteral("STANDARD_IA");

        const Result<PutObjectResult> r =
            await<PutObjectResult>([&](auto cb) { client->putObject(p, std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->etag, QStringLiteral("\"e1\""));
        QCOMPARE(r->versionId, QStringLiteral("v7"));
        QVERIFY(r->requestId.startsWith(QLatin1String("FAKE")));   // for the journal

        const RecordedRequest req = m_server->requests().first();
        QCOMPARE(req.path, QByteArray("/bkt/dir/file%20name.bin"));
        QCOMPARE(req.body, p.data);
        QCOMPARE(req.header("Content-MD5"), md5Base64(p.data));
        QCOMPARE(req.header("Content-Type"), QByteArray("application/octet-stream"));
        QCOMPARE(req.header("x-amz-meta-owner"), QByteArray("rene"));
        QCOMPARE(req.header("x-amz-storage-class"), QByteArray("STANDARD_IA"));
        // Signed with the real payload hash (verified by the server).
        QVERIFY(req.header("x-amz-content-sha256") != "UNSIGNED-PAYLOAD");
    }

    void putObjectRejectsNonAsciiMetadata()
    {
        auto client = makeClient();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("k");
        p.metadata.insert(QStringLiteral("note"), QStringLiteral("grüße"));
        const Result<PutObjectResult> r =
            await<PutObjectResult>([&](auto cb) { client->putObject(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void getObjectVerifiesMd5()
    {
        const QByteArray content = "hello object";
        m_server->on("GET", QStringLiteral("^/bkt/good$"),
                     FakeResponse::ok(content, "text/plain").withHeader("ETag", "\"" + md5Hex(content) + "\""));
        m_server->on("GET", QStringLiteral("^/bkt/corrupt$"),
                     FakeResponse::ok(content, "text/plain").withHeader("ETag", "\"" + md5Hex("other") + "\""));
        m_server->on("GET", QStringLiteral("^/bkt/multipart$"),
                     FakeResponse::ok(content, "text/plain").withHeader("ETag", "\"0123456789abcdef0123456789abcdef-3\""));
        m_server->on("GET", QStringLiteral("^/bkt/kms$"),
                     FakeResponse::ok(content, "text/plain")
                         .withHeader("ETag", "\"" + md5Hex("other") + "\"")
                         .withHeader("x-amz-server-side-encryption", "aws:kms"));
        auto client = makeClient();
        const auto get = [&](const char* key) {
            return await<ObjectData>([&](auto cb) {
                client->getObject(QStringLiteral("bkt"), QString::fromLatin1(key), {}, std::move(cb));
            });
        };

        Result<ObjectData> r = get("good");
        QVERIFY(r);
        QCOMPARE(r->data, content);
        QCOMPARE(r->integrity, Integrity::Md5Verified);
        QCOMPARE(r->meta.size, content.size());

        r = get("corrupt");
        QCOMPARE(r.error().category, ErrorCategory::Protocol);

        r = get("multipart");
        QVERIFY(r);
        QCOMPARE(r->integrity, Integrity::SizeVerified);

        r = get("kms");
        QVERIFY(r);
        QCOMPARE(r->integrity, Integrity::SizeVerified);
    }

    void getObjectReturnsXmlErrorDocumentsAsData()
    {
        // A user's file that happens to look like an S3 error.
        const QByteArray content = "<Error><Code>InternalError</Code></Error>";
        m_server->on("GET", QString(), FakeResponse::ok(content, kXml));
        auto client = makeClient();
        const Result<ObjectData> r = await<ObjectData>([&](auto cb) {
            client->getObject(QStringLiteral("bkt"), QStringLiteral("error.xml"), {}, std::move(cb));
        });
        QVERIFY(r);
        QCOMPARE(r->data, content);
    }

    void deleteObjectReportsMarkers()
    {
        m_server->on("DELETE", QString(),
                     FakeResponse().withHeader("x-amz-delete-marker", "true").withHeader("x-amz-version-id", "dm1"));
        auto client = makeClient();
        const Result<DeleteObjectResult> r = await<DeleteObjectResult>([&](auto cb) {
            client->deleteObject(QStringLiteral("bkt"), QStringLiteral("k"), QStringLiteral("v1"), std::move(cb));
        });
        QVERIFY(r);
        QVERIFY(r->deleteMarker);
        QCOMPARE(r->versionId, QStringLiteral("dm1"));
        QCOMPARE(m_server->requests().first().queryItem("versionId"), QByteArray("v1"));
    }

    void deleteObjectsBatch()
    {
        const QByteArray result = "<DeleteResult>"
                                  "<Deleted><Key>a&amp;b</Key></Deleted>"
                                  "<Deleted><Key>c</Key><DeleteMarker>true</DeleteMarker>"
                                  "<DeleteMarkerVersionId>dm</DeleteMarkerVersionId></Deleted>"
                                  "<Error><Key>d</Key><Code>AccessDenied</Code><Message>no</Message></Error>"
                                  "</DeleteResult>";
        Matcher m;
        m.method = "POST";
        m.requiredQueryKeys = {"delete"};
        m_server->addRoute(m, {FakeResponse::ok(result, kXml)});
        auto client = makeClient();
        const QList<ObjectIdentifier> objects = {
            {QStringLiteral("a&b"), {}}, {QStringLiteral("c"), {}},
            {QStringLiteral("d"), {}}, {QStringLiteral("silent"), {}},
        };
        const Result<DeleteObjectsResult> r = await<DeleteObjectsResult>([&](auto cb) {
            client->deleteObjects(QStringLiteral("bkt"), objects, std::move(cb));
        });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->deleted.size(), 2);
        QCOMPARE(r->failed.size(), 2);
        QCOMPARE(r->failed[0].code, QStringLiteral("AccessDenied"));
        QCOMPARE(r->failed[1].key, QStringLiteral("silent"));
        QCOMPARE(r->failed[1].code, QStringLiteral("NotReported"));

        const RecordedRequest req = m_server->requests().first();
        QCOMPARE(req.header("Content-MD5"), md5Base64(req.body));
        QCOMPARE(keysIn(req.body),
                 (QStringList{QStringLiteral("a&b"), QStringLiteral("c"), QStringLiteral("d"),
                              QStringLiteral("silent")}));
        QVERIFY(!req.body.contains("<Quiet>"));
    }

    void deleteObjectsValidation()
    {
        auto client = makeClient();
        QList<ObjectIdentifier> tooMany;
        for (int i = 0; i < 1001; ++i)
            tooMany.append({QStringLiteral("k%1").arg(i), {}});
        Result<DeleteObjectsResult> r = await<DeleteObjectsResult>([&](auto cb) {
            client->deleteObjects(QStringLiteral("bkt"), tooMany, std::move(cb));
        });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);

        r = await<DeleteObjectsResult>([&](auto cb) {
            client->deleteObjects(QStringLiteral("bkt"), {}, std::move(cb));
        });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);

        r = await<DeleteObjectsResult>([&](auto cb) {
            client->deleteObjects(QStringLiteral("bkt"), {{QStringLiteral("bell\x07"), {}}}, std::move(cb));
        });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void deleteObjectsErrorIn200()
    {
        Matcher m;
        m.requiredQueryKeys = {"delete"};
        m_server->addRoute(m, {FakeResponse::ok("<Error><Code>InternalError</Code><Message>x</Message></Error>", kXml)});
        auto client = makeClient();
        const Result<DeleteObjectsResult> r = await<DeleteObjectsResult>([&](auto cb) {
            client->deleteObjects(QStringLiteral("bkt"), {{QStringLiteral("k"), {}}}, std::move(cb));
        });
        QVERIFY(!r);
        QCOMPARE(r.error().code, QStringLiteral("InternalError"));
        QCOMPARE(r.error().category, ErrorCategory::Transient);
        QCOMPARE(r.error().httpStatus, 200);
    }

    void createAndDeleteBucket()
    {
        m_server->on("PUT", QString(), FakeResponse());
        m_server->on("DELETE", QStringLiteral("^/full$"), FakeResponse::s3Error(409, "BucketNotEmpty"));
        m_server->on("DELETE", QString(), FakeResponse());
        auto client = makeClient();

        CreateBucketParams p;
        p.bucket = QStringLiteral("new-bucket");
        p.region = QStringLiteral("fsn1");
        p.objectLockEnabled = true;
        QVERIFY(await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); }));
        RecordedRequest req = m_server->requests().last();
        QCOMPARE(req.path, QByteArray("/new-bucket"));
        QVERIFY(req.body.contains("<LocationConstraint>fsn1</LocationConstraint>"));
        QCOMPARE(req.header("x-amz-bucket-object-lock-enabled"), QByteArray("true"));

        p.region = QStringLiteral("us-east-1");
        p.objectLockEnabled = false;
        QVERIFY(await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); }));
        req = m_server->requests().last();
        QVERIFY(req.body.isEmpty());
        QVERIFY(req.header("x-amz-bucket-object-lock-enabled").isNull());

        QVERIFY(await<void>([&](auto cb) { client->deleteBucket(QStringLiteral("old"), std::move(cb)); }));
        const Result<void> full =
            await<void>([&](auto cb) { client->deleteBucket(QStringLiteral("full"), std::move(cb)); });
        QCOMPARE(full.error().category, ErrorCategory::Conflict);
    }

    void copyObjectSendsTheSourceAndKeepsTheClass()
    {
        QFile f(QStringLiteral(TEST_DATA_DIR "/generic/copy-object-result.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        m_server->on("PUT", QString(), FakeResponse::ok(f.readAll(), kXml).withHeader("x-amz-version-id", "v9"));
        auto client = makeClient();
        CopyObjectParams p;
        p.sourceBucket = QStringLiteral("bkt");
        p.sourceKey = QStringLiteral("old dir/a+b ü.txt");
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("new dir/a+b ü.txt");
        p.ifMatch = QStringLiteral("\"9b2cf535f27731c974343645a3985328\"");
        p.storageClass = QStringLiteral("STANDARD_IA");
        const Result<CopyObjectResult> r =
            await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->etag, QStringLiteral("\"9b2cf535f27731c974343645a3985328\""));
        QCOMPARE(r->lastModified, QDateTime(QDate(2026, 10, 20), QTime(8, 15, 30), QTimeZone::UTC));
        QCOMPARE(r->versionId, QStringLiteral("v9"));
        QVERIFY(!r->requestId.isEmpty());

        const RecordedRequest req = m_server->requests().first();
        QCOMPARE(req.method, QByteArray("PUT"));
        QCOMPARE(req.path, QByteArray("/bkt/new%20dir/a%2Bb%20%C3%BC.txt"));
        QCOMPARE(req.header("x-amz-copy-source"), QByteArray("/bkt/old%20dir/a%2Bb%20%C3%BC.txt"));
        QCOMPARE(req.header("x-amz-metadata-directive"), QByteArray("COPY"));
        QCOMPARE(req.header("x-amz-copy-source-if-match"), p.ifMatch.toLatin1());
        QCOMPARE(req.header("x-amz-storage-class"), QByteArray("STANDARD_IA"));
        QVERIFY(req.body.isEmpty());
    }

    void copyObjectFailuresAreNeverSuccesses()
    {
        auto client = makeClient();
        CopyObjectParams p;
        p.sourceBucket = QStringLiteral("bkt");
        p.sourceKey = QStringLiteral("a");
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("b");

        // 200 with an <Error> body: the copy failed after the headers went out.
        m_server->on("PUT", QString(), FakeResponse::ok("<Error><Code>InternalError</Code><Message>x</Message></Error>", kXml), 1);
        Result<CopyObjectResult> r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QVERIFY(!r);
        QCOMPARE(r.error().httpStatus, 200);
        QCOMPARE(r.error().code, QStringLiteral("InternalError"));
        QCOMPARE(m_server->requestCount(), 1);   // not retried: not idempotent

        // The source changed since the plan.
        m_server->on("PUT", QString(), FakeResponse::s3Error(412, "PreconditionFailed"), 1);
        p.ifMatch = QStringLiteral("\"old\"");
        r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::PreconditionFailed);

        // A lost answer: the copy may exist. Unknown, and sent once — by QNAM
        // too, which would otherwise send it again by itself (OneShotBody).
        m_server->clearRequests();
        FakeResponse drop;
        drop.dropWithoutResponse = true;
        m_server->on("PUT", QString(), drop);
        r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QVERIFY(!r);
        QVERIFY(r.error().outcomeUnknown);
        QCOMPARE(m_server->requestCount(), 1);
    }

    void copyObjectRefusesWhatCannotBeSent()
    {
        auto client = makeClient();
        CopyObjectParams p;
        p.sourceBucket = QStringLiteral("bkt");
        p.sourceKey = QStringLiteral("a");
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("a");   // onto itself
        Result<CopyObjectResult> r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        p.key = QStringLiteral("x/../a");
        r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    // An edit of headers / metadata / class is a copy onto itself with
    // REPLACE: everything the object keeps must be sent — and only that.
    void copyInPlaceReplacesEverything()
    {
        QFile f(QStringLiteral(TEST_DATA_DIR "/generic/copy-object-result.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        m_server->on("PUT", QString(), FakeResponse::ok(f.readAll(), kXml));
        auto client = makeClient();
        CopyObjectParams p;
        p.sourceBucket = p.bucket = QStringLiteral("bkt");
        p.sourceKey = p.key = QStringLiteral("site/index.html");
        p.ifMatch = QStringLiteral("\"9b2cf535f27731c974343645a3985328\"");
        p.metadataDirective = MetadataDirective::Replace;
        p.headers.contentType = QStringLiteral("text/html; charset=utf-8");
        p.headers.cacheControl = QStringLiteral("max-age=60");
        p.headers.contentDisposition = QStringLiteral("attachment; filename=\"index.html\"");
        p.headers.contentLanguage = QStringLiteral("de");
        p.headers.expires = QStringLiteral("Wed, 21 Oct 2026 07:28:00 GMT");
        p.headers.websiteRedirectLocation = QStringLiteral("/other.html");
        p.headers.userMetadata.insert(QStringLiteral("Origin"), QStringLiteral("edited"));
        p.serverSideEncryption = QStringLiteral("AES256");
        p.cannedAcl = QStringLiteral("public-read");
        Result<CopyObjectResult> r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));

        const RecordedRequest req = m_server->requests().first();
        QCOMPARE(req.header("x-amz-copy-source"), QByteArray("/bkt/site/index.html"));
        QCOMPARE(req.header("x-amz-metadata-directive"), QByteArray("REPLACE"));
        QCOMPARE(req.header("content-type"), QByteArray("text/html; charset=utf-8"));
        QCOMPARE(req.header("cache-control"), QByteArray("max-age=60"));
        QCOMPARE(req.header("content-disposition"), QByteArray("attachment; filename=\"index.html\""));
        QCOMPARE(req.header("content-language"), QByteArray("de"));
        QCOMPARE(req.header("expires"), QByteArray("Wed, 21 Oct 2026 07:28:00 GMT"));
        QCOMPARE(req.header("x-amz-website-redirect-location"), QByteArray("/other.html"));
        QCOMPARE(req.header("x-amz-meta-origin"), QByteArray("edited"));
        QCOMPARE(req.header("x-amz-server-side-encryption"), QByteArray("AES256"));
        QCOMPARE(req.header("x-amz-acl"), QByteArray("public-read"));
        QVERIFY(req.header("content-encoding").isNull());   // not set: not sent, so it's gone

        // Only the class changes: allowed onto itself, the directive stays COPY.
        m_server->clearRequests();
        CopyObjectParams cls;
        cls.sourceBucket = cls.bucket = QStringLiteral("bkt");
        cls.sourceKey = cls.key = QStringLiteral("a");
        cls.storageClass = QStringLiteral("STANDARD_IA");
        r = await<CopyObjectResult>([&](auto cb) { client->copyObject(cls, std::move(cb)); });
        QVERIFY(r);
        QCOMPARE(m_server->requests().first().header("x-amz-metadata-directive"), QByteArray("COPY"));
        QVERIFY(m_server->requests().first().header("cache-control").isNull());

        // A header that can't be sent byte-exact is refused before sending.
        m_server->clearRequests();
        p.headers.contentDisposition = QStringLiteral("attachment; filename=\"übersicht.pdf\"");
        r = await<CopyObjectResult>([&](auto cb) { client->copyObject(p, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void headReportsWhatACopyMustCarry()
    {
        m_server->on("HEAD", QString(), FakeResponse::ok()
                                             .withHeader("ETag", "\"9b2cf535f27731c974343645a3985328\"")
                                             .withHeader("Content-Type", "text/plain")
                                             .withHeader("Cache-Control", "no-cache")
                                             .withHeader("Content-Disposition", "inline")
                                             .withHeader("Content-Encoding", "gzip")
                                             .withHeader("Content-Language", "en")
                                             .withHeader("Expires", "Thu, 01 Jan 2027 00:00:00 GMT")
                                             .withHeader("x-amz-website-redirect-location", "/x")
                                             .withHeader("x-amz-meta-Owner", "ops")
                                             .withHeader("x-amz-server-side-encryption", "aws:kms")
                                             .withHeader("x-amz-server-side-encryption-aws-kms-key-id", "key-1")
                                             .withHeader("x-amz-tagging-count", "3")
                                             .withHeader("x-amz-object-lock-mode", "GOVERNANCE")
                                             .withHeader("x-amz-object-lock-retain-until-date", "2027-01-01T00:00:00.000Z")
                                             .withHeader("x-amz-object-lock-legal-hold", "ON"));
        auto client = makeClient();
        const Result<ObjectMeta> m =
            await<ObjectMeta>([&](auto cb) { client->headObject(QStringLiteral("bkt"), QStringLiteral("k"), {}, std::move(cb)); });
        QVERIFY2(m, qPrintable(m ? QString() : m.error().toString()));
        QCOMPARE(m->cacheControl, QStringLiteral("no-cache"));
        QCOMPARE(m->contentDisposition, QStringLiteral("inline"));
        QCOMPARE(m->contentEncoding, QStringLiteral("gzip"));
        QCOMPARE(m->contentLanguage, QStringLiteral("en"));
        QCOMPARE(m->expires, QStringLiteral("Thu, 01 Jan 2027 00:00:00 GMT"));
        QCOMPARE(m->websiteRedirectLocation, QStringLiteral("/x"));
        QCOMPARE(m->serverSideEncryption, QStringLiteral("aws:kms"));
        QCOMPARE(m->sseKmsKeyId, QStringLiteral("key-1"));
        QVERIFY(m->sseCustomerAlgorithm.isEmpty());
        QCOMPARE(m->tagCount, 3);
        QCOMPARE(m->objectLockMode, QStringLiteral("GOVERNANCE"));
        QCOMPARE(m->objectLockRetainUntil, QDateTime(QDate(2027, 1, 1), QTime(0, 0), QTimeZone::UTC));
        QVERIFY(m->legalHold);
        const ObjectHeaders h = objectHeadersOf(*m);
        QCOMPARE(h.contentType, QStringLiteral("text/plain"));
        QCOMPARE(h.userMetadata.value(QStringLiteral("owner")), QStringLiteral("ops"));
        QCOMPARE(h.contentEncoding, QStringLiteral("gzip"));
    }

    void lostRepliesOfNonIdempotentRequests()
    {
        FakeResponse drop;
        drop.dropWithoutResponse = true;
        m_server->on("PUT", QString(), drop);
        auto client = makeClient();

        CreateBucketParams p;
        p.bucket = QStringLiteral("maybe");
        const Result<void> create = await<void>([&](auto cb) { client->createBucket(p, std::move(cb)); });
        QCOMPARE(create.error().category, ErrorCategory::Transient);
        QVERIFY(create.error().outcomeUnknown);

        PutObjectParams put;
        put.bucket = QStringLiteral("bkt");
        put.key = QStringLiteral("k");
        put.data = "x";
        const Result<PutObjectResult> r =
            await<PutObjectResult>([&](auto cb) { client->putObject(put, std::move(cb)); });
        QCOMPARE(r.error().category, ErrorCategory::Transient);
        QVERIFY(!r.error().outcomeUnknown);   // PUT of the same bytes is idempotent
    }
};

QTEST_GUILESS_MAIN(TestS3ClientWrite)
#include "test_s3client_write.moc"
