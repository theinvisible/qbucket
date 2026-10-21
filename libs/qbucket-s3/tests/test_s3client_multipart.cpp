// Multipart uploads, part-number reads and server-side part copies through
// the Session, against FakeObjectStore.

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>

#include <functional>
#include <optional>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

constexpr qint64 kKiB = 1024;

const QString kBucket = QStringLiteral("bkt");

Credentials testCredentials()
{
    Credentials c;
    c.accessKeyId = QStringLiteral("AKIDTEST");
    c.secretAccessKey = QStringLiteral("test/secret+key");
    return c;
}

QByteArray pattern(qint64 size, quint32 seed = 2463534242u)
{
    QByteArray out(size, Qt::Uninitialized);
    quint32 x = seed;
    for (qint64 i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = char(x & 0xFF);
    }
    return out;
}

QByteArray md5(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5);
}

FileBody bodyFor(const QString& path, const QByteArray& data, qint64 offset, qint64 length)
{
    FileBody b;
    b.path = path;
    b.offset = offset;
    b.length = length;
    const QByteArray range = data.mid(offset, length);
    b.md5Base64 = md5(range).toBase64();
    b.sha256Hex = QCryptographicHash::hash(range, QCryptographicHash::Sha256).toHex();
    return b;
}

// Waits for one callback; an Internal error on timeout.
template <typename T>
Result<T> await(const std::function<void(Callback<T>)>& start, int timeoutMs = 10000)
{
    std::optional<Result<T>> out;
    start([&out](Result<T> r) { out.emplace(std::move(r)); });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, timeoutMs))
        return Error::make(ErrorCategory::Internal, QStringLiteral("timed out"));
    return std::move(*out);
}

#define REQUIRE_OK(r)                                       \
    do {                                                    \
        if (!(r))                                           \
            QFAIL(qPrintable((r).error().toString()));      \
    } while (false)

} // namespace

class TestS3ClientMultipart : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<QTemporaryDir> m_dir;
    QObject m_context;
    QByteArray m_data;
    QString m_path;

    std::unique_ptr<Session> makeSession(int lanes = 2, int transferTimeoutMs = 10000, int serverWorkTimeoutMs = 10000)
    {
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.transferTimeoutMs = transferTimeoutMs;
        o.serverWorkTimeoutMs = serverWorkTimeoutMs;
        o.retryBaseDelayMs = 10;
        o.transferLanes = lanes;
        return std::make_unique<Session>(o);
    }

    Result<CreateMultipartUploadResult> create(const QString& key)
    {
        CreateMultipartUploadParams p;
        p.bucket = kBucket;
        p.key = key;
        p.headers.contentType = QStringLiteral("video/mp2t");
        p.headers.userMetadata.insert(QStringLiteral("origin"), QStringLiteral("test"));
        return await<CreateMultipartUploadResult>([&](auto cb) { m_session->createMultipartUpload(p, &m_context, cb); });
    }

    Result<UploadPartResult> part(const QString& key, const QString& uploadId, int number, qint64 offset, qint64 length)
    {
        UploadPartParams p;
        p.bucket = kBucket;
        p.key = key;
        p.uploadId = uploadId;
        p.partNumber = number;
        p.file = bodyFor(m_path, m_data, offset, length);
        return await<UploadPartResult>([&](auto cb) { m_session->uploadPart(p, &m_context, cb); });
    }

    Result<CompleteMultipartUploadResult> complete(const QString& key, const QString& uploadId,
                                                   const QList<CompletedPart>& parts, bool ifNoneMatch = false)
    {
        CompleteMultipartUploadParams p;
        p.bucket = kBucket;
        p.key = key;
        p.uploadId = uploadId;
        p.parts = parts;
        p.ifNoneMatch = ifNoneMatch;
        return await<CompleteMultipartUploadResult>([&](auto cb) { m_session->completeMultipartUpload(p, &m_context, cb); });
    }

    // Uploads m_data in three parts (40 KiB, 40 KiB, rest) under `key`.
    QList<CompletedPart> uploadThreeParts(const QString& key, QString* uploadId)
    {
        const auto created = create(key);
        if (!created)
            return {};
        *uploadId = created->uploadId;
        QList<CompletedPart> parts;
        const qint64 sizes[] = {40 * kKiB, 40 * kKiB, m_data.size() - 80 * kKiB};
        qint64 offset = 0;
        for (int i = 0; i < 3; ++i) {
            const auto r = part(key, *uploadId, i + 1, offset, sizes[i]);
            if (!r)
                return {};
            parts.append({r->partNumber, r->etag});
            offset += sizes[i];
        }
        return parts;
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
        m_store->setMinPartSize(32 * kKiB);
        m_session = makeSession();
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_data = pattern(100 * kKiB + 123);
        m_path = m_dir->filePath(QStringLiteral("src.bin"));
        QFile f(m_path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(m_data), m_data.size());
    }

    void cleanup()
    {
        m_session.reset();
        m_store.reset();
        delete m_server;
        m_server = nullptr;
        m_dir.reset();
    }

    void roundTrip()
    {
        QString uploadId;
        const QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("dir/movie.ts"), &uploadId);
        QCOMPARE(parts.size(), 3);
        QCOMPARE(parts[0].etag, QLatin1Char('"') + QString::fromLatin1(md5(m_data.left(40 * kKiB)).toHex()) + QLatin1Char('"'));

        const auto done = complete(QStringLiteral("dir/movie.ts"), uploadId, parts);
        REQUIRE_OK(done);
        const QString expected = FakeObjectStore::compositeEtag(
            {md5(m_data.left(40 * kKiB)), md5(m_data.mid(40 * kKiB, 40 * kKiB)), md5(m_data.mid(80 * kKiB))});
        QCOMPARE(done->etag, expected);
        QVERIFY(done->etag.endsWith(QStringLiteral("-3\"")));
        QVERIFY(m_store->uploadIds(kBucket).isEmpty());

        const auto stored = m_store->object(kBucket, QStringLiteral("dir/movie.ts"));
        QVERIFY(stored);
        QCOMPARE(stored->data, m_data);
        QCOMPARE(stored->contentType, QStringLiteral("video/mp2t"));
        QCOMPARE(stored->metadata.value(QStringLiteral("origin")), QStringLiteral("test"));

        // Every part request carried its Content-MD5.
        for (const RecordedRequest& r : m_server->requests()) {
            if (r.method == "PUT")
                QVERIFY(!r.header("content-md5").isEmpty());
        }
    }

    void partLayoutByPartNumber()
    {
        QString uploadId;
        const QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("obj"), &uploadId);
        REQUIRE_OK(complete(QStringLiteral("obj"), uploadId, parts));

        HeadObjectParams h;
        h.bucket = kBucket;
        h.key = QStringLiteral("obj");
        h.partNumber = 1;
        const auto meta = await<ObjectMeta>([&](auto cb) { m_session->headObject(h, &m_context, cb); });
        REQUIRE_OK(meta);
        QCOMPARE(meta->partsCount, 3);
        QCOMPARE(meta->size, 40 * kKiB);

        GetObjectToFileParams g;
        g.bucket = kBucket;
        g.key = QStringLiteral("obj");
        g.partNumber = 3;
        g.filePath = m_dir->filePath(QStringLiteral("part3"));
        g.ifMatch = meta->etag;
        const auto got = await<DownloadResult>([&](auto cb) { m_session->getObjectToFile(g, &m_context, cb); });
        REQUIRE_OK(got);
        QCOMPARE(got->bytesWritten, m_data.size() - 80 * kKiB);
        QCOMPARE(got->md5, md5(m_data.mid(80 * kKiB)));
        QCOMPARE(got->partsCount, 3);
        QCOMPARE(got->objectSize, m_data.size());
        QCOMPARE(got->integrity, Integrity::SizeVerified);
    }

    void listPartsPagesThrough()
    {
        const auto created = create(QStringLiteral("obj"));
        REQUIRE_OK(created);
        for (int n = 1; n <= 5; ++n)
            REQUIRE_OK(part(QStringLiteral("obj"), created->uploadId, n, qint64(n - 1) * 10 * kKiB, 10 * kKiB));

        QList<int> seen;
        ListPartsParams p;
        p.bucket = kBucket;
        p.key = QStringLiteral("obj");
        p.uploadId = created->uploadId;
        p.maxParts = 2;
        for (int page = 0; page < 10; ++page) {
            const auto r = await<ListPartsPage>([&](auto cb) { m_session->listParts(p, &m_context, cb); });
            REQUIRE_OK(r);
            for (const PartInfo& i : r->parts) {
                seen.append(i.partNumber);
                QCOMPARE(i.size, 10 * kKiB);
            }
            if (!r->isTruncated)
                break;
            p.partNumberMarker = r->nextPartNumberMarker;
        }
        QCOMPARE(seen, (QList<int>{1, 2, 3, 4, 5}));
    }

    void listPartsLoopIsAnError()
    {
        m_server->on("GET", QString(),
                     FakeResponse::ok("<ListPartsResult><IsTruncated>true</IsTruncated><NextPartNumberMarker>4"
                                      "</NextPartNumberMarker></ListPartsResult>"));
        ListPartsParams p;
        p.bucket = kBucket;
        p.key = QStringLiteral("obj");
        p.uploadId = QStringLiteral("u");
        p.partNumberMarker = 4;
        const auto r = await<ListPartsPage>([&](auto cb) { m_session->listParts(p, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
    }

    void listUploadsAndAbort()
    {
        QStringList ids;
        for (const QString& key : {QStringLiteral("b key"), QStringLiteral("a"), QStringLiteral("b key")}) {
            const auto c = create(key);
            REQUIRE_OK(c);
            ids.append(c->uploadId);
        }
        ListMultipartUploadsParams p;
        p.bucket = kBucket;
        p.maxUploads = 1;
        QList<MultipartUploadInfo> all;
        for (int page = 0; page < 10; ++page) {
            const auto r = await<ListMultipartUploadsPage>([&](auto cb) { m_session->listMultipartUploads(p, &m_context, cb); });
            REQUIRE_OK(r);
            all += r->uploads;
            if (!r->isTruncated)
                break;
            p.keyMarker = r->nextKeyMarker;
            p.uploadIdMarker = r->nextUploadIdMarker;
        }
        QCOMPARE(all.size(), 3);
        QCOMPARE(all[0].key, QStringLiteral("a"));
        QCOMPARE(all[1].key, QStringLiteral("b key"));
        QCOMPARE(all[2].key, QStringLiteral("b key"));
        QVERIFY(all[0].initiated.isValid());

        const auto aborted = await<void>([&](auto cb) {
            m_session->abortMultipartUpload(kBucket, QStringLiteral("a"), ids[1], &m_context, cb);
        });
        REQUIRE_OK(aborted);
        QCOMPARE(m_store->uploadIds(kBucket).size(), 2);
        // Gone: NotFound, which a caller treats as "already aborted".
        const auto again = await<void>([&](auto cb) {
            m_session->abortMultipartUpload(kBucket, QStringLiteral("a"), ids[1], &m_context, cb);
        });
        QVERIFY(!again);
        QCOMPARE(again.error().category, ErrorCategory::NotFound);
        QCOMPARE(again.error().code, QStringLiteral("NoSuchUpload"));
    }

    void corruptPartIsRejected()
    {
        const auto created = create(QStringLiteral("obj"));
        REQUIRE_OK(created);
        UploadPartParams p;
        p.bucket = kBucket;
        p.key = QStringLiteral("obj");
        p.uploadId = created->uploadId;
        p.partNumber = 1;
        p.file = bodyFor(m_path, m_data, 0, 10 * kKiB);
        p.file.md5Base64 = md5("something else").toBase64();
        const auto r = await<UploadPartResult>([&](auto cb) { m_session->uploadPart(p, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().code, QStringLiteral("BadDigest"));
        QVERIFY(m_store->uploadParts(created->uploadId).isEmpty());
    }

    void completeChecksTheParts()
    {
        QString uploadId;
        QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("obj"), &uploadId);
        QList<CompletedPart> wrong = parts;
        wrong[1].etag = QStringLiteral("\"00000000000000000000000000000000\"");
        const auto r = complete(QStringLiteral("obj"), uploadId, wrong);
        QVERIFY(!r);
        QCOMPARE(r.error().code, QStringLiteral("InvalidPart"));
        QVERIFY(!m_store->object(kBucket, QStringLiteral("obj")));

        // Out of order: refused before anything is sent.
        const int before = m_server->requestCount();
        const auto unordered = complete(QStringLiteral("obj"), uploadId, {parts[1], parts[0]});
        QVERIFY(!unordered);
        QCOMPARE(unordered.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), before);
    }

    void completeWith200ErrorFails()
    {
        QString uploadId;
        const QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("obj"), &uploadId);
        QFile f(QStringLiteral(TEST_DATA_DIR "/generic/complete-multipart-upload-error.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        m_store->addFault({QStringLiteral("CompleteMultipartUpload"), 0, 0, 1, false, FakeResponse::ok(f.readAll())});
        const auto r = complete(QStringLiteral("obj"), uploadId, parts);
        QVERIFY(!r);
        QCOMPARE(r.error().code, QStringLiteral("InternalError"));
        QCOMPARE(m_store->count(QStringLiteral("CompleteMultipartUpload")), 1);   // not retried
        QVERIFY(!m_store->object(kBucket, QStringLiteral("obj")));
    }

    void lostCompleteAnswerIsOutcomeUnknown()
    {
        QString uploadId;
        const QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("obj"), &uploadId);
        FakeResponse lost;
        lost.dropWithoutResponse = true;
        m_store->addFault({QStringLiteral("CompleteMultipartUpload"), 0, 0, 1, true, lost});
        const auto r = complete(QStringLiteral("obj"), uploadId, parts);
        QVERIFY(!r);
        QVERIFY(r.error().outcomeUnknown);
        QCOMPARE(m_store->count(QStringLiteral("CompleteMultipartUpload")), 1);
        // It did land: a caller resolves this with HEAD.
        const auto meta = await<ObjectMeta>([&](auto cb) {
            m_session->headObject(kBucket, QStringLiteral("obj"), {}, &m_context, cb);
        });
        REQUIRE_OK(meta);
        QVERIFY(meta->etag.endsWith(QStringLiteral("-3\"")));
    }

    // Assembling a big object takes the server minutes without a byte on the
    // wire (VersityGW, 50 GiB): Complete gets the longer server-work timeout,
    // ordinary requests keep theirs.
    void slowCompleteGetsTheServerWorkTimeout()
    {
        QString uploadId;
        const QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("slow"), &uploadId);
        m_session = makeSession(2, 400, 10000);
        m_store->setDelay(QStringLiteral("CompleteMultipartUpload"), 1500);
        REQUIRE_OK(complete(QStringLiteral("slow"), uploadId, parts));
        QCOMPARE(m_store->count(QStringLiteral("CompleteMultipartUpload")), 1);

        m_store->setDelay(QStringLiteral("HeadObject"), 1500);
        const auto meta = await<ObjectMeta>([&](auto cb) {
            m_session->headObject(kBucket, QStringLiteral("slow"), {}, &m_context, cb);
        });
        QVERIFY(!meta);
        QVERIFY(meta.error().isTransient());
    }

    void ifNoneMatchRefusesOverwrites()
    {
        m_store->putObject(kBucket, QStringLiteral("taken"), "old");
        PutObjectParams put;
        put.bucket = kBucket;
        put.key = QStringLiteral("taken");
        put.file = bodyFor(m_path, m_data, 0, 100);
        put.ifNoneMatch = true;
        const auto r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("taken"))->data, QByteArray("old"));
        QCOMPARE(m_server->requests().last().header("if-none-match"), QByteArray("*"));

        QString uploadId;
        const QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("taken"), &uploadId);
        const auto c = complete(QStringLiteral("taken"), uploadId, parts, true);
        QVERIFY(!c);
        QCOMPARE(c.error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("taken"))->data, QByteArray("old"));

        put.key = QStringLiteral("free");
        const auto fresh = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        REQUIRE_OK(fresh);
    }

    void ifMatchReplacesOnlyTheSameObject()
    {
        m_store->putObject(kBucket, QStringLiteral("doc"), "old");
        const QString etag = m_store->object(kBucket, QStringLiteral("doc"))->etag;
        PutObjectParams put;
        put.bucket = kBucket;
        put.key = QStringLiteral("doc");
        put.file = bodyFor(m_path, m_data, 0, 100);
        put.ifMatch = QStringLiteral("\"0123456789abcdef0123456789abcdef\"");
        auto r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->data, QByteArray("old"));
        QCOMPARE(m_server->requests().last().header("if-match"), QByteArray("\"0123456789abcdef0123456789abcdef\""));

        put.key = QStringLiteral("missing");
        r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::NotFound);
        QVERIFY(!m_store->object(kBucket, QStringLiteral("missing")));

        put.key = QStringLiteral("doc");
        put.ifMatch = etag;
        REQUIRE_OK(await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); }));
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->data, m_data.left(100));

        // The same for a multipart upload, decided by its Complete.
        const QString current = m_store->object(kBucket, QStringLiteral("doc"))->etag;
        QString uploadId;
        QList<CompletedPart> parts = uploadThreeParts(QStringLiteral("doc"), &uploadId);
        CompleteMultipartUploadParams p;
        p.bucket = kBucket;
        p.key = QStringLiteral("doc");
        p.uploadId = uploadId;
        p.parts = parts;
        p.ifMatch = etag;   // stale now
        auto c = await<CompleteMultipartUploadResult>([&](auto cb) { m_session->completeMultipartUpload(p, &m_context, cb); });
        QVERIFY(!c);
        QCOMPARE(c.error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->data, m_data.left(100));
        p.ifMatch = current;
        REQUIRE_OK(await<CompleteMultipartUploadResult>([&](auto cb) { m_session->completeMultipartUpload(p, &m_context, cb); }));
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->size, m_data.size());
    }

    void conditionsExcludeEachOther()
    {
        PutObjectParams put;
        put.bucket = kBucket;
        put.key = QStringLiteral("k");
        put.file = bodyFor(m_path, m_data, 0, 100);
        put.ifNoneMatch = true;
        put.ifMatch = QStringLiteral("\"e\"");
        auto r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        put.ifNoneMatch = false;
        put.ifMatch.clear();
        put.headers = ObjectHeaders{};
        put.contentType = QStringLiteral("text/plain");
        r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void unsupportedConditionalOverwriteIsRefusedLocally()
    {
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::ConditionalOverwrite, {CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        m_store->putObject(kBucket, QStringLiteral("doc"), "old");
        PutObjectParams put;
        put.bucket = kBucket;
        put.key = QStringLiteral("doc");
        put.file = bodyFor(m_path, m_data, 0, 100);
        put.ifMatch = m_store->object(kBucket, QStringLiteral("doc"))->etag;
        const auto r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QCOMPARE(m_server->requestCount(), 0);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->data, QByteArray("old"));
    }

    void putObjectCarriesEveryHeader()
    {
        PutObjectParams put;
        put.bucket = kBucket;
        put.key = QStringLiteral("page.html");
        put.file = bodyFor(m_path, m_data, 0, 100);
        ObjectHeaders h;
        h.contentType = QStringLiteral("text/html; charset=utf-8");
        h.cacheControl = QStringLiteral("max-age=60");
        h.contentDisposition = QStringLiteral("inline");
        h.contentEncoding = QStringLiteral("identity");
        h.contentLanguage = QStringLiteral("de");
        h.expires = QStringLiteral("Wed, 21 Oct 2026 07:28:00 GMT");
        h.userMetadata.insert(QStringLiteral("origin"), QStringLiteral("editor"));
        put.headers = h;
        put.storageClass = QStringLiteral("STANDARD");
        put.serverSideEncryption = QStringLiteral("AES256");
        put.cannedAcl = QStringLiteral("public-read");
        REQUIRE_OK(await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); }));
        const auto o = m_store->object(kBucket, QStringLiteral("page.html"));
        QVERIFY(o);
        QCOMPARE(o->contentType, h.contentType);
        QCOMPARE(o->headers.value(QStringLiteral("cache-control")), h.cacheControl);
        QCOMPARE(o->headers.value(QStringLiteral("content-disposition")), h.contentDisposition);
        QCOMPARE(o->headers.value(QStringLiteral("content-encoding")), h.contentEncoding);
        QCOMPARE(o->headers.value(QStringLiteral("content-language")), h.contentLanguage);
        QCOMPARE(o->headers.value(QStringLiteral("expires")), h.expires);
        QCOMPARE(o->headers.value(QStringLiteral("x-amz-server-side-encryption")), QStringLiteral("AES256"));
        QCOMPARE(o->metadata.value(QStringLiteral("origin")), QStringLiteral("editor"));
        QCOMPARE(o->acl, QStringLiteral("public-read"));
    }

    void rangedGetObjectInMemory()
    {
        m_store->putObject(kBucket, QStringLiteral("blob"), m_data.left(1000));
        const QString etag = m_store->object(kBucket, QStringLiteral("blob"))->etag;
        GetObjectParams g;
        g.bucket = kBucket;
        g.key = QStringLiteral("blob");
        g.rangeStart = 100;
        g.rangeLength = 200;
        g.ifMatch = etag;
        auto r = await<ObjectData>([&](auto cb) { m_session->getObject(g, &m_context, cb); });
        REQUIRE_OK(r);
        QCOMPARE(r->data, m_data.mid(100, 200));
        QCOMPARE(r->objectSize, 1000);
        QCOMPARE(r->integrity, Integrity::SizeVerified);
        QCOMPARE(m_server->requests().last().header("range"), QByteArray("bytes=100-299"));

        // The tail: fewer bytes than asked for, up to the end.
        g.rangeStart = 900;
        g.rangeLength = 500;
        r = await<ObjectData>([&](auto cb) { m_session->getObject(g, &m_context, cb); });
        REQUIRE_OK(r);
        QCOMPARE(r->data, m_data.mid(900, 100));

        g.ifMatch = QStringLiteral("\"0123456789abcdef0123456789abcdef\"");
        r = await<ObjectData>([&](auto cb) { m_session->getObject(g, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::PreconditionFailed);

        // The whole object through the same call is MD5-checked.
        g.ifMatch.clear();
        g.rangeStart = 0;
        g.rangeLength = -1;
        r = await<ObjectData>([&](auto cb) { m_session->getObject(g, &m_context, cb); });
        REQUIRE_OK(r);
        QCOMPARE(r->integrity, Integrity::Md5Verified);
        QCOMPARE(r->objectSize, 1000);

        m_store->setIgnoreRanges(true);
        g.rangeStart = 10;
        g.rangeLength = 10;
        r = await<ObjectData>([&](auto cb) { m_session->getObject(g, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
    }

    void unsupportedConditionalWritesAreRefusedLocally()
    {
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::ConditionalWrites, {CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        PutObjectParams put;
        put.bucket = kBucket;
        put.key = QStringLiteral("k");
        put.file = bodyFor(m_path, m_data, 0, 100);
        put.ifNoneMatch = true;
        const auto r = await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QCOMPARE(m_server->requestCount(), 0);
        // Without the condition it goes through.
        put.ifNoneMatch = false;
        REQUIRE_OK(await<PutObjectResult>([&](auto cb) { m_session->putObject(put, &m_context, cb); }));
    }

    void unsupportedMultipartIsRefusedOnEveryLane()
    {
        // Set before any transfer lane exists: lanes started later get it too.
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::MultipartUpload, {CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        UploadPartParams p;
        p.bucket = kBucket;
        p.key = QStringLiteral("k");
        p.uploadId = QStringLiteral("u");
        p.partNumber = 1;
        p.file = bodyFor(m_path, m_data, 0, 100);
        const auto r = await<UploadPartResult>([&](auto cb) { m_session->uploadPart(p, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        const auto c = create(QStringLiteral("k"));
        QVERIFY(!c);
        QCOMPARE(c.error().category, ErrorCategory::Unsupported);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void throttledPartIsRetriedAndReported()
    {
        const auto created = create(QStringLiteral("obj"));
        REQUIRE_OK(created);
        m_store->addFault({QStringLiteral("UploadPart"), 2, 0, 1, false, FakeResponse::s3Error(503, "SlowDown")});
        const auto first = part(QStringLiteral("obj"), created->uploadId, 1, 0, 10 * kKiB);
        REQUIRE_OK(first);
        QVERIFY(!first->throttled);
        const auto second = part(QStringLiteral("obj"), created->uploadId, 2, 10 * kKiB, 10 * kKiB);
        REQUIRE_OK(second);
        QVERIFY(second->throttled);
        QCOMPARE(m_store->count(QStringLiteral("UploadPart")), 3);
    }

    void uploadPartCopyAssemblesRanges()
    {
        m_store->putObject(kBucket, QStringLiteral("source"), m_data);
        const auto created = create(QStringLiteral("copy"));
        REQUIRE_OK(created);
        const auto sourceEtag = m_store->object(kBucket, QStringLiteral("source"))->etag;
        QList<CompletedPart> parts;
        const qint64 half = 50 * kKiB;
        for (int n = 1; n <= 2; ++n) {
            UploadPartCopyParams p;
            p.sourceBucket = kBucket;
            p.sourceKey = QStringLiteral("source");
            p.bucket = kBucket;
            p.key = QStringLiteral("copy");
            p.uploadId = created->uploadId;
            p.partNumber = n;
            p.rangeStart = n == 1 ? 0 : half;
            p.rangeLength = n == 1 ? half : m_data.size() - half;
            p.ifMatch = sourceEtag;
            const auto r = await<UploadPartCopyResult>([&](auto cb) { m_session->uploadPartCopy(p, &m_context, cb); });
            REQUIRE_OK(r);
            QCOMPARE(r->partNumber, n);
            parts.append({n, r->etag});
        }
        REQUIRE_OK(complete(QStringLiteral("copy"), created->uploadId, parts));
        QCOMPARE(m_store->object(kBucket, QStringLiteral("copy"))->data, m_data);
        const QList<RecordedRequest> requests = m_server->requests();
        bool sawRange = false;
        for (const RecordedRequest& r : requests)
            sawRange |= r.header("x-amz-copy-source-range") == "bytes=0-51199";
        QVERIFY(sawRange);

        // The source changed: the if-match stops the part copy.
        UploadPartCopyParams stale;
        stale.sourceBucket = kBucket;
        stale.sourceKey = QStringLiteral("source");
        stale.bucket = kBucket;
        stale.key = QStringLiteral("copy2");
        stale.uploadId = create(QStringLiteral("copy2"))->uploadId;
        stale.partNumber = 1;
        stale.ifMatch = QStringLiteral("\"0123\"");
        const auto r = await<UploadPartCopyResult>([&](auto cb) { m_session->uploadPartCopy(stale, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::PreconditionFailed);
    }

    void uploadPartCopyWith200ErrorFails()
    {
        m_store->putObject(kBucket, QStringLiteral("source"), m_data);
        const auto created = create(QStringLiteral("copy"));
        REQUIRE_OK(created);
        m_store->addFault({QStringLiteral("UploadPartCopy"), 0, 0, -1, false,
                           FakeResponse::ok("<Error><Code>AccessDenied</Code><Message>no</Message></Error>")});
        UploadPartCopyParams p;
        p.sourceBucket = kBucket;
        p.sourceKey = QStringLiteral("source");
        p.bucket = kBucket;
        p.key = QStringLiteral("copy");
        p.uploadId = created->uploadId;
        p.partNumber = 1;
        const auto r = await<UploadPartCopyResult>([&](auto cb) { m_session->uploadPartCopy(p, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::NotPermitted);
    }

    void crossBucketPartCopyNeedsTheFeature()
    {
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::CrossBucketCopy, {CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        UploadPartCopyParams p;
        p.sourceBucket = QStringLiteral("other");
        p.sourceKey = QStringLiteral("source");
        p.bucket = kBucket;
        p.key = QStringLiteral("copy");
        p.uploadId = QStringLiteral("u");
        p.partNumber = 1;
        const auto r = await<UploadPartCopyResult>([&](auto cb) { m_session->uploadPartCopy(p, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void invalidPartNumbersAreRefused()
    {
        UploadPartParams p;
        p.bucket = kBucket;
        p.key = QStringLiteral("k");
        p.uploadId = QStringLiteral("u");
        p.file = bodyFor(m_path, m_data, 0, 100);
        for (const int n : {0, 10001}) {
            p.partNumber = n;
            const auto r = await<UploadPartResult>([&](auto cb) { m_session->uploadPart(p, &m_context, cb); });
            QVERIFY(!r);
            QCOMPARE(r.error().category, ErrorCategory::InvalidRequest);
        }
        QCOMPARE(m_server->requestCount(), 0);
    }

    // FakeObjectStore's CopyObject, which the app's copy / edit tests rely on.
    void objectStoreCopiesLikeS3()
    {
        m_store->putObject(kBucket, QStringLiteral("src"), "content", QStringLiteral("text/plain"));
        CopyObjectParams edit;
        edit.sourceBucket = edit.bucket = kBucket;
        edit.sourceKey = edit.key = QStringLiteral("src");
        edit.metadataDirective = MetadataDirective::Replace;
        edit.headers.contentType = QStringLiteral("text/markdown");
        edit.headers.cacheControl = QStringLiteral("max-age=5");
        edit.headers.userMetadata.insert(QStringLiteral("k"), QStringLiteral("v"));
        edit.cannedAcl = QStringLiteral("public-read");
        REQUIRE_OK(await<CopyObjectResult>([&](auto cb) { m_session->copyObject(edit, &m_context, cb); }));
        auto o = m_store->object(kBucket, QStringLiteral("src"));
        QCOMPARE(o->data, QByteArray("content"));
        QCOMPARE(o->contentType, QStringLiteral("text/markdown"));
        QCOMPARE(o->headers.value(QStringLiteral("cache-control")), QStringLiteral("max-age=5"));
        QCOMPARE(o->metadata.value(QStringLiteral("k")), QStringLiteral("v"));
        QCOMPARE(o->acl, QStringLiteral("public-read"));

        // COPY keeps headers and metadata, never the ACL; HEAD reports them.
        CopyObjectParams copy;
        copy.sourceBucket = copy.bucket = kBucket;
        copy.sourceKey = QStringLiteral("src");
        copy.key = QStringLiteral("dst");
        REQUIRE_OK(await<CopyObjectResult>([&](auto cb) { m_session->copyObject(copy, &m_context, cb); }));
        o = m_store->object(kBucket, QStringLiteral("dst"));
        QCOMPARE(o->acl, QStringLiteral("private"));
        const auto meta = await<ObjectMeta>([&](auto cb) {
            m_session->headObject(kBucket, QStringLiteral("dst"), {}, &m_context, cb);
        });
        REQUIRE_OK(meta);
        QCOMPARE(meta->contentType, QStringLiteral("text/markdown"));
        QCOMPARE(meta->cacheControl, QStringLiteral("max-age=5"));
        QCOMPARE(meta->userMetadata.value(QStringLiteral("k")), QStringLiteral("v"));

        // A server that ignores REPLACE keeps the old headers.
        m_store->setIgnoreMetadataReplace(true);
        edit.sourceKey = edit.key = QStringLiteral("dst");
        edit.headers.cacheControl = QStringLiteral("no-store");
        REQUIRE_OK(await<CopyObjectResult>([&](auto cb) { m_session->copyObject(edit, &m_context, cb); }));
        QCOMPARE(m_store->object(kBucket, QStringLiteral("dst"))->headers.value(QStringLiteral("cache-control")),
                 QStringLiteral("max-age=5"));
    }

    void tagsAndAclsRoundTrip()
    {
        m_store->putObject(kBucket, QStringLiteral("obj"), "data");
        PutObjectTaggingParams tags;
        tags.bucket = kBucket;
        tags.key = QStringLiteral("obj");
        tags.tags = {{QStringLiteral("project"), QStringLiteral("qbucket")}, {QStringLiteral("tier"), QStringLiteral("hot")}};
        REQUIRE_OK(await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectTagging(tags, &m_context, cb); }));
        auto read = await<ObjectTagging>([&](auto cb) {
            m_session->getObjectTagging(kBucket, QStringLiteral("obj"), {}, &m_context, cb);
        });
        REQUIRE_OK(read);
        QCOMPARE(read->tags, tags.tags);
        const auto meta = await<ObjectMeta>([&](auto cb) {
            m_session->headObject(kBucket, QStringLiteral("obj"), {}, &m_context, cb);
        });
        REQUIRE_OK(meta);
        QCOMPARE(meta->tagCount, 2);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("obj"))->data, QByteArray("data"));   // content untouched

        // An empty set removes them (no DeleteObjectTagging).
        tags.tags.clear();
        REQUIRE_OK(await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectTagging(tags, &m_context, cb); }));
        read = await<ObjectTagging>([&](auto cb) { m_session->getObjectTagging(kBucket, QStringLiteral("obj"), {}, &m_context, cb); });
        QVERIFY(read->tags.isEmpty());

        auto acl = await<AccessControlPolicy>([&](auto cb) {
            m_session->getObjectAcl(kBucket, QStringLiteral("obj"), {}, &m_context, cb);
        });
        REQUIRE_OK(acl);
        QCOMPARE(cannedAclOf(*acl), std::optional<QString>(QStringLiteral("private")));
        PutObjectAclParams pub;
        pub.bucket = kBucket;
        pub.key = QStringLiteral("obj");
        pub.cannedAcl = QStringLiteral("public-read");
        REQUIRE_OK(await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectAcl(pub, &m_context, cb); }));
        acl = await<AccessControlPolicy>([&](auto cb) { m_session->getObjectAcl(kBucket, QStringLiteral("obj"), {}, &m_context, cb); });
        QCOMPARE(cannedAclOf(*acl), std::optional<QString>(QStringLiteral("public-read")));

        // Invalid tag sets never leave the client.
        tags.tags = {{QStringLiteral("aws:x"), QString()}};
        const int before = m_server->requestCount();
        QCOMPARE(await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectTagging(tags, &m_context, cb); })
                     .error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), before);
    }

    // A proxy that drops the subresource stores the request as the object.
    // The answer carries an ETag then — reported, never a success.
    void subresourceWriteAnsweredLikeAnUploadIsAnError()
    {
        m_store->putObject(kBucket, QStringLiteral("victim"), "precious");
        m_store->setDropObjectSubresources(true);
        PutObjectTaggingParams tags;
        tags.bucket = kBucket;
        tags.key = QStringLiteral("victim");
        tags.tags = {{QStringLiteral("k"), QStringLiteral("v")}};
        const auto r = await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectTagging(tags, &m_context, cb); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
        QVERIFY(r.error().message.contains(QLatin1String("versions")));
        QVERIFY(m_store->object(kBucket, QStringLiteral("victim"))->data.startsWith("<?xml"));   // why it matters
        PutObjectAclParams acl;
        acl.bucket = kBucket;
        acl.key = QStringLiteral("victim");
        acl.cannedAcl = QStringLiteral("private");
        QCOMPARE(await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectAcl(acl, &m_context, cb); })
                     .error().category, ErrorCategory::Protocol);
        // And a GET of the subresource that comes back as the object is no tag set.
        const auto read = await<ObjectTagging>([&](auto cb) {
            m_session->getObjectTagging(kBucket, QStringLiteral("victim"), {}, &m_context, cb);
        });
        QVERIFY(!read);
    }

    void writeFeaturesAreCheckedAndLearnedSeparately()
    {
        m_store->putObject(kBucket, QStringLiteral("obj"), "data");
        // Write support Unsupported: refused locally — also as a header of a copy.
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::ObjectAclWrite, {CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        CopyObjectParams copy;
        copy.sourceBucket = copy.bucket = kBucket;
        copy.sourceKey = QStringLiteral("obj");
        copy.key = QStringLiteral("copy");
        copy.cannedAcl = QStringLiteral("public-read");
        const int before = m_server->requestCount();
        auto r = await<CopyObjectResult>([&](auto cb) { m_session->copyObject(copy, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QCOMPARE(m_server->requestCount(), before);
        copy.cannedAcl.clear();
        REQUIRE_OK(await<CopyObjectResult>([&](auto cb) { m_session->copyObject(copy, &m_context, cb); }));
        // Reading the ACL is a different feature and still works.
        REQUIRE_OK(await<AccessControlPolicy>([&](auto cb) {
            m_session->getObjectAcl(kBucket, QStringLiteral("obj"), {}, &m_context, cb);
        }));

        // A 501 on PutObjectTagging teaches the write feature only.
        m_server->addRoute({"PUT", {}, {"tagging"}, {}}, {FakeResponse::s3Error(501, "NotImplemented")}, 1);
        QSignalSpy learned(m_session.get(), &Session::capabilityLearned);
        PutObjectTaggingParams tags;
        tags.bucket = kBucket;
        tags.key = QStringLiteral("obj");
        QCOMPARE(await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectTagging(tags, &m_context, cb); })
                     .error().category, ErrorCategory::Unsupported);
        QTRY_COMPARE(learned.size(), 1);
        QCOMPARE(learned.first().at(1).toString(), QStringLiteral("object.tagging-write"));
        REQUIRE_OK(await<ObjectTagging>([&](auto cb) {
            m_session->getObjectTagging(kBucket, QStringLiteral("obj"), {}, &m_context, cb);
        }));
    }

    void presignHonoursTheCapability()
    {
        PresignRequest req;
        req.bucket = kBucket;
        req.key = QStringLiteral("k");
        req.expiresSeconds = 60;
        REQUIRE_OK(await<QUrl>([&](auto cb) { m_session->presign(req, &m_context, cb); }));
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::Presign, {CapabilityState::Unsupported, CapabilitySource::Probed, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        QCOMPARE(await<QUrl>([&](auto cb) { m_session->presign(req, &m_context, cb); }).error().category,
                 ErrorCategory::Unsupported);
    }

    void objectStoreListsLikeS3()
    {
        // FakeObjectStore's ListObjectsV2, which later tests rely on.
        for (const QString& k : {QStringLiteral("a/1"), QStringLiteral("a/2"), QStringLiteral("a-b"),
                                 QStringLiteral("b"), QStringLiteral("c d/x")})
            m_store->putObject(kBucket, k, "x");
        ListObjectsParams p;
        p.bucket = kBucket;
        p.delimiter = QStringLiteral("/");
        p.maxKeys = 2;
        QStringList seen;
        for (int page = 0; page < 10; ++page) {
            const auto r = await<ListPage>([&](auto cb) { m_session->listObjects(p, &m_context, cb); });
            REQUIRE_OK(r);
            for (const ObjectEntry& e : r->objects)
                seen.append(e.key);
            seen += r->commonPrefixes;
            if (!r->isTruncated)
                break;
            p.continuationToken = r->nextContinuationToken;
        }
        seen.sort();
        QCOMPARE(seen, (QStringList{QStringLiteral("a-b"), QStringLiteral("a/"), QStringLiteral("b"), QStringLiteral("c d/")}));
    }
};

QTEST_GUILESS_MAIN(TestS3ClientMultipart)
#include "test_s3client_multipart.moc"
