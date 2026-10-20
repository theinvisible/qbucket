// Integration tests against a real S3 endpoint — opt-in, skipped unless
// QBUCKET_IT_ENDPOINT is set (PLAN.md §7):
//
//   QBUCKET_IT_ENDPOINT, QBUCKET_IT_REGION, QBUCKET_IT_ACCESS_KEY,
//   QBUCKET_IT_SECRET_KEY, QBUCKET_IT_BUCKET (must match qbucket-it-*),
//   QBUCKET_IT_ADDRESSING (path|virtual), QBUCKET_IT_PROVIDER, QBUCKET_IT_BACKEND,
//   optional: QBUCKET_IT_CREATE_BUCKET=1 (local backends only),
//             QBUCKET_IT_RESTRICTED_ACCESS_KEY / _SECRET_KEY (a user without
//             access to the bucket, for NotPermitted)
//
// Known deviations of a backend live in tests/data/it-expectations/
// <backend>.json. They are asserted, not just skipped — when a backend
// changes, the test notices.
//
// Safety: refuses any bucket not named qbucket-it-*; every key lives under
// it/<run id>/ and only that prefix is cleaned up. The bucket itself is
// never deleted.

#include <qbucket/s3/ListObjectsPager.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/ProviderRegistry.h>
#include <qbucket/s3/transfer/Copier.h>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;

namespace {

QString env(const char* name)
{
    return qEnvironmentVariable(name);
}

template <typename T, typename Start>
Result<T> await(Start start, int timeoutMs = 60000)
{
    std::optional<Result<T>> out;
    start([&out](Result<T> r) { out.emplace(std::move(r)); });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, timeoutMs))
        return Error::make(ErrorCategory::Internal, QStringLiteral("test timeout"));
    return std::move(*out);
}

// Evaluates `result` exactly once — it is usually a request.
#define VERIFY_OK(result)                                                                   \
    do {                                                                                    \
        const auto& verifyOkResult_ = (result);                                             \
        QVERIFY2(verifyOkResult_,                                                           \
                 qPrintable(verifyOkResult_ ? QString() : verifyOkResult_.error().toString())); \
    } while (false)

// Keys that broke S3 clients before: encoding, Unicode, delimiters, XML.
QStringList nastyKeys()
{
    return {
        QStringLiteral("plain.txt"),
        QStringLiteral("space and+plus.txt"),
        QStringLiteral("100%25 literal percent%.txt"),
        QStringLiteral("sub-delims !$&'()*,;=:@.txt"),
        QStringLiteral("query?and#hash.txt"),
        QStringLiteral("unicode äöüß 文件 \U0001F600.txt"),
        QStringLiteral("nfd café.txt"),
        QStringLiteral("dir/nested/deep.txt"),
        QStringLiteral("dir//double-slash.txt"),
        QStringLiteral("tilde~and_under-score.txt"),
        QStringLiteral("xml <&>\"'.txt"),
        QStringLiteral("brackets [x]{y}.txt"),
        QStringLiteral(".hidden"),
        QStringLiteral("trailing-dot."),
        QStringLiteral("line\nbreak.txt"),
        QStringLiteral("tab\tseparated.txt"),
    };
}

} // namespace

class ItS3Backend : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<Session> m_session;
    SessionOptions m_options;
    QString m_bucket;
    QString m_prefix;      // it/<run id>/
    QString m_backend;
    QJsonObject m_expect;   // known deviations of this backend
    QObject m_ctx;
    QTemporaryDir m_dir;

    bool expects(const char* quirk) const { return m_expect.value(QLatin1String(quirk)).toBool(); }

    SessionOptions optionsWith(const QString& accessKey, const QString& secret) const
    {
        SessionOptions o = m_options;
        Credentials c;
        c.accessKeyId = accessKey;
        c.secretAccessKey = secret;
        o.credentials = std::make_shared<StaticCredentialsProvider>(c);
        return o;
    }

    QString key(const QString& name) const { return m_prefix + name; }

    Result<PutObjectResult> put(const QString& k, const QByteArray& data)
    {
        PutObjectParams p;
        p.bucket = m_bucket;
        p.key = k;
        p.data = data;
        p.contentType = QStringLiteral("text/plain");
        return await<PutObjectResult>([&](auto cb) { m_session->putObject(p, &m_ctx, std::move(cb)); });
    }

    // Every key under `prefix`, walking all pages.
    Result<QStringList> listKeys(const QString& prefix, int maxKeys = 1000)
    {
        ListObjectsParams p;
        p.bucket = m_bucket;
        p.prefix = prefix;
        p.maxKeys = maxKeys;
        ListObjectsPager pager(*m_session, p);
        QStringList keys;
        while (pager.hasMore()) {
            Result<ListPage> page = await<ListPage>([&](auto cb) { pager.fetchNext(&m_ctx, std::move(cb)); });
            if (!page)
                return page.error();
            for (const ObjectEntry& e : page->objects)
                keys.append(e.key);
        }
        return keys;
    }

    // A local file with `data`, and a FileBody for a range of it.
    QString writeFile(const QString& name, const QByteArray& data)
    {
        QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return {};
        return path;
    }

    static FileBody bodyFor(const QString& path, const QByteArray& data, qint64 offset, qint64 length)
    {
        FileBody b;
        b.path = path;
        b.offset = offset;
        b.length = length;
        const QByteArray range = data.mid(offset, length);
        b.md5Base64 = QCryptographicHash::hash(range, QCryptographicHash::Md5).toBase64();
        b.sha256Hex = QCryptographicHash::hash(range, QCryptographicHash::Sha256).toHex();
        return b;
    }

    // Aborts every unfinished upload this run left under its prefix.
    void abortAllUnder(const QString& prefix)
    {
        ListMultipartUploadsParams p;
        p.bucket = m_bucket;
        p.prefix = prefix;
        for (int page = 0; page < 100; ++page) {
            const Result<ListMultipartUploadsPage> r = await<ListMultipartUploadsPage>(
                [&](auto cb) { m_session->listMultipartUploads(p, &m_ctx, std::move(cb)); });
            if (!r)
                return;
            for (const MultipartUploadInfo& u : r->uploads) {
                if (u.key.startsWith(m_prefix)) {
                    (void)await<void>([&](auto cb) {
                        m_session->abortMultipartUpload(m_bucket, u.key, u.uploadId, &m_ctx, std::move(cb));
                    });
                }
            }
            if (!r->isTruncated)
                return;
            p.keyMarker = r->nextKeyMarker;
            p.uploadIdMarker = r->nextUploadIdMarker;
        }
    }

    void deleteAllUnder(const QString& prefix)
    {
        const Result<QStringList> keys = listKeys(prefix);
        if (!keys)
            return;
        QList<ObjectIdentifier> batch;
        for (const QString& k : keys.value()) {
            if (!k.startsWith(m_prefix))
                continue;   // never touch anything outside our run
            batch.append({k, {}});
            if (batch.size() == 1000) {
                (void)await<DeleteObjectsResult>([&](auto cb) { m_session->deleteObjects(m_bucket, batch, &m_ctx, std::move(cb)); });
                batch.clear();
            }
        }
        if (!batch.isEmpty())
            (void)await<DeleteObjectsResult>([&](auto cb) { m_session->deleteObjects(m_bucket, batch, &m_ctx, std::move(cb)); });
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (env("QBUCKET_IT_ENDPOINT").isEmpty())
            QSKIP("QBUCKET_IT_ENDPOINT not set — integration tests are opt-in");

        m_bucket = env("QBUCKET_IT_BUCKET");
        static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
        QVERIFY2(safeName.match(m_bucket).hasMatch(),
                 "refusing to run: QBUCKET_IT_BUCKET must be a dedicated qbucket-it-* bucket");
        m_backend = env("QBUCKET_IT_BACKEND");
        QFile expectations(QStringLiteral(TEST_DATA_DIR "/it-expectations/") + m_backend
                           + QStringLiteral(".json"));
        if (!m_backend.isEmpty() && expectations.open(QIODevice::ReadOnly))
            m_expect = QJsonDocument::fromJson(expectations.readAll()).object();

        m_options.endpoint.url = QUrl(env("QBUCKET_IT_ENDPOINT"));
        m_options.endpoint.region = env("QBUCKET_IT_REGION").isEmpty() ? QStringLiteral("us-east-1")
                                                                         : env("QBUCKET_IT_REGION");
        m_options.endpoint.addressing = env("QBUCKET_IT_ADDRESSING") == QLatin1String("virtual")
            ? AddressingStyle::VirtualHosted
            : AddressingStyle::Path;
        m_options.profile = ProviderRegistry::builtin().findOrGeneric(env("QBUCKET_IT_PROVIDER"));
        m_options.transferTimeoutMs = 30000;
        m_options = optionsWith(env("QBUCKET_IT_ACCESS_KEY"), env("QBUCKET_IT_SECRET_KEY"));
        QVERIFY2(m_options.endpoint.isValid(), qPrintable(m_options.endpoint.validationError()));
        m_session = std::make_unique<Session>(m_options);

        const quint64 run = QRandomGenerator::global()->generate64();
        m_prefix = QStringLiteral("it/%1/").arg(run, 16, 16, QLatin1Char('0'));
        qInfo().noquote() << "backend" << m_backend << "bucket" << m_bucket << "prefix" << m_prefix;

        if (env("QBUCKET_IT_CREATE_BUCKET") == QLatin1String("1")) {
            CreateBucketParams p;
            p.bucket = m_bucket;
            p.region = m_options.endpoint.region;
            const Result<void> r = await<void>([&](auto cb) { m_session->createBucket(p, &m_ctx, std::move(cb)); });
            QVERIFY2(r || r.error().category == ErrorCategory::Conflict, qPrintable(r ? QString() : r.error().toString()));
        }
    }

    void cleanupTestCase()
    {
        if (m_session) {
            abortAllUnder(m_prefix);
            deleteAllUnder(m_prefix);
        }
    }

    void bucketIsVisible()
    {
        const Result<BucketList> list = await<BucketList>([&](auto cb) { m_session->listBuckets(&m_ctx, std::move(cb)); });
        VERIFY_OK(list);
        const bool found = std::any_of(list->buckets.cbegin(), list->buckets.cend(),
                                       [&](const BucketInfo& b) { return b.name == m_bucket; });
        QVERIFY(found);

        VERIFY_OK((await<void>([&](auto cb) { m_session->headBucket(m_bucket, &m_ctx, std::move(cb)); })));

        const Result<QString> region = await<QString>([&](auto cb) { m_session->getBucketLocation(m_bucket, &m_ctx, std::move(cb)); });
        VERIFY_OK(region);
        QVERIFY(!region->isEmpty());
    }

    void objectRoundTrip()
    {
        const QByteArray data = QByteArray("integration payload\n").repeated(1000);
        const QString k = key(QStringLiteral("roundtrip.txt"));
        const Result<PutObjectResult> put = this->put(k, data);
        VERIFY_OK(put);
        QCOMPARE(put->etag.toLatin1(),
                 '"' + QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex() + '"');

        const Result<ObjectMeta> head = await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(head);
        QCOMPARE(head->size, data.size());

        const Result<ObjectData> get = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(get);
        QCOMPARE(get->data, data);
        QCOMPARE(get->integrity, Integrity::Md5Verified);

        VERIFY_OK((await<DeleteObjectResult>([&](auto cb) { m_session->deleteObject(m_bucket, k, {}, &m_ctx, std::move(cb)); })));
        const Result<ObjectMeta> gone = await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        QVERIFY(!gone);
        QCOMPARE(gone.error().category, ErrorCategory::NotFound);
    }

    void copyObjectKeepsContentAndChecksIfMatch()
    {
        const QByteArray data = QByteArray("copy me\n").repeated(500);
        const QString src = key(QStringLiteral("copy src/ä+b.txt"));
        const QString dst = key(QStringLiteral("copy dst/ä+b.txt"));
        const Result<PutObjectResult> put = this->put(src, data);
        VERIFY_OK(put);

        CopyObjectParams p;
        p.sourceBucket = m_bucket;
        p.sourceKey = src;
        p.bucket = m_bucket;
        p.key = dst;
        p.ifMatch = put->etag;
        const Result<CopyObjectResult> copied =
            await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
        VERIFY_OK(copied);
        QCOMPARE(copied->etag, put->etag);   // single part: the content's MD5
        const Result<ObjectData> get = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, dst, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(get);
        QCOMPARE(get->data, data);
        QCOMPARE(get->meta.contentType, QStringLiteral("text/plain"));   // metadata directive COPY

        // A stale ETag: refused, nothing written.
        p.key = key(QStringLiteral("copy dst/never.txt"));
        p.ifMatch = QStringLiteral("\"00000000000000000000000000000000\"");
        const Result<CopyObjectResult> stale =
            await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
        QVERIFY(!stale);
        QCOMPARE(stale.error().category, ErrorCategory::PreconditionFailed);
        const Result<ObjectMeta> none = await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, p.key, {}, &m_ctx, std::move(cb)); });
        QVERIFY(!none);
    }

    // Two parts (5 MiB + a tail), streamed from a file: ListParts, the
    // composite ETag, part-number reads where the backend has them, ranged
    // and whole downloads into files.
    void multipartRoundTrip()
    {
        const qint64 first = Q_INT64_C(5) * 1024 * 1024;
        const QByteArray data = QByteArray("multipart payload 0123456789\n").repeated(int(first / 29) + 200);
        const QString path = writeFile(QStringLiteral("multipart.bin"), data);
        QVERIFY(!path.isEmpty());
        const QString k = key(QStringLiteral("multipart/ä big.bin"));
        qInfo() << "payload hash required:" << m_session->requiresPayloadHash();

        CreateMultipartUploadParams cp;
        cp.bucket = m_bucket;
        cp.key = k;
        cp.contentType = QStringLiteral("application/x-test");
        cp.metadata.insert(QStringLiteral("origin"), QStringLiteral("it"));
        const Result<CreateMultipartUploadResult> created =
            await<CreateMultipartUploadResult>([&](auto cb) { m_session->createMultipartUpload(cp, &m_ctx, std::move(cb)); });
        VERIFY_OK(created);

        QList<CompletedPart> parts;
        QList<QByteArray> md5s;
        const qint64 offsets[] = {0, first};
        const qint64 lengths[] = {first, data.size() - first};
        for (int i = 0; i < 2; ++i) {
            UploadPartParams up;
            up.bucket = m_bucket;
            up.key = k;
            up.uploadId = created->uploadId;
            up.partNumber = i + 1;
            up.file = bodyFor(path, data, offsets[i], lengths[i]);
            const Result<UploadPartResult> r =
                await<UploadPartResult>([&](auto cb) { m_session->uploadPart(up, &m_ctx, std::move(cb)); });
            VERIFY_OK(r);
            const QByteArray md5 = QCryptographicHash::hash(data.mid(offsets[i], lengths[i]), QCryptographicHash::Md5);
            QCOMPARE(r->etag.toLatin1(), '"' + md5.toHex() + '"');
            parts.append({r->partNumber, r->etag});
            md5s.append(md5);
        }

        ListPartsParams lp;
        lp.bucket = m_bucket;
        lp.key = k;
        lp.uploadId = created->uploadId;
        const Result<ListPartsPage> listed = await<ListPartsPage>([&](auto cb) { m_session->listParts(lp, &m_ctx, std::move(cb)); });
        VERIFY_OK(listed);
        QCOMPARE(listed->parts.size(), 2);
        QCOMPARE(listed->parts[0].size, first);
        QCOMPARE(listed->parts[1].etag, parts[1].etag);

        CompleteMultipartUploadParams cmp;
        cmp.bucket = m_bucket;
        cmp.key = k;
        cmp.uploadId = created->uploadId;
        cmp.parts = parts;
        const Result<CompleteMultipartUploadResult> done = await<CompleteMultipartUploadResult>(
            [&](auto cb) { m_session->completeMultipartUpload(cmp, &m_ctx, std::move(cb)); });
        VERIFY_OK(done);
        QByteArray all;
        for (const QByteArray& m : md5s)
            all += m;
        const QString composite = QLatin1Char('"') + QString::fromLatin1(QCryptographicHash::hash(all, QCryptographicHash::Md5).toHex())
                                + QStringLiteral("-2\"");
        QCOMPARE(done->etag, composite);

        const Result<ObjectMeta> head = await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(head);
        QCOMPARE(head->size, data.size());
        QCOMPARE(head->etag, composite);
        QCOMPARE(head->contentType, QStringLiteral("application/x-test"));
        QCOMPARE(head->userMetadata.value(QStringLiteral("origin")), QStringLiteral("it"));

        // The part layout. Not every backend answers part-number requests.
        HeadObjectParams hp;
        hp.bucket = m_bucket;
        hp.key = k;
        hp.partNumber = 1;
        const Result<ObjectMeta> part1 = await<ObjectMeta>([&](auto cb) { m_session->headObject(hp, &m_ctx, std::move(cb)); });
        const bool layout = part1 && part1->partsCount == 2 && part1->size == first;
        qInfo() << "HEAD ?partNumber=1:" << (part1 ? QStringLiteral("parts %1, size %2").arg(part1->partsCount).arg(part1->size)
                                                    : part1.error().toString());
        const QJsonValue expectLayout = m_expect.value(QLatin1String("partNumberReads"));
        if (expectLayout.isBool())
            QCOMPARE(layout, expectLayout.toBool());

        // Whole object into a file: verified by size here (multipart ETag).
        GetObjectToFileParams gp;
        gp.bucket = m_bucket;
        gp.key = k;
        gp.filePath = m_dir.filePath(QStringLiteral("multipart.download"));
        gp.ifMatch = composite;
        const Result<DownloadResult> whole = await<DownloadResult>([&](auto cb) { m_session->getObjectToFile(gp, &m_ctx, std::move(cb)); });
        VERIFY_OK(whole);
        QCOMPARE(whole->bytesWritten, data.size());
        QCOMPARE(whole->md5, QCryptographicHash::hash(data, QCryptographicHash::Md5));

        // A range, written at its offset.
        gp.filePath = m_dir.filePath(QStringLiteral("multipart.range"));
        gp.fileOffset = 1000;
        gp.rangeStart = first - 10;
        gp.rangeLength = 30;
        const Result<DownloadResult> range = await<DownloadResult>([&](auto cb) { m_session->getObjectToFile(gp, &m_ctx, std::move(cb)); });
        VERIFY_OK(range);
        QCOMPARE(range->objectSize, data.size());
        QFile f(gp.filePath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll().mid(1000), data.mid(first - 10, 30));

        // The second part by number, where supported.
        if (layout) {
            gp.filePath = m_dir.filePath(QStringLiteral("multipart.part2"));
            gp.fileOffset = 0;
            gp.rangeLength = -1;
            gp.partNumber = 2;
            const Result<DownloadResult> p2 = await<DownloadResult>([&](auto cb) { m_session->getObjectToFile(gp, &m_ctx, std::move(cb)); });
            VERIFY_OK(p2);
            QCOMPARE(p2->md5, md5s[1]);
        }
    }

    // UploadPartCopy through transfer::Copier: an object copied in two
    // ranges, content and metadata intact.
    void multipartCopy()
    {
        const QByteArray data = QByteArray("copy in parts 0123456789\n").repeated(260000);   // 6.2 MB
        const QString src = key(QStringLiteral("partcopy/src.bin"));
        PutObjectParams p;
        p.bucket = m_bucket;
        p.key = src;
        p.data = data;
        p.contentType = QStringLiteral("application/x-partcopy");
        p.metadata.insert(QStringLiteral("origin"), QStringLiteral("it"));
        const Result<PutObjectResult> put = await<PutObjectResult>([&](auto cb) { m_session->putObject(p, &m_ctx, std::move(cb)); });
        VERIFY_OK(put);

        transfer::CopyRequest r;
        r.sourceBucket = m_bucket;
        r.sourceKey = src;
        r.bucket = m_bucket;
        r.key = key(QStringLiteral("partcopy/dst.bin"));
        r.ifMatch = put->etag;
        r.partSize = Q_INT64_C(5) * 1024 * 1024;
        transfer::Copier copier(m_session.get(), r, nullptr);
        QSignalSpy finished(&copier, &transfer::Copier::finished);
        copier.start();
        QVERIFY(finished.wait(120000));
        VERIFY_OK(copier.result());
        QCOMPARE(copier.result()->parts, 2);
        const Result<ObjectData> got = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, r.key, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(got);
        QCOMPARE(got->data, data);
        QCOMPARE(got->meta.contentType, QStringLiteral("application/x-partcopy"));
        QCOMPARE(got->meta.userMetadata.value(QStringLiteral("origin")), QStringLiteral("it"));
    }

    // The safety case (PLAN.md §12, Hetzner's proxy once dropped a query
    // string): a multipart upload and an aborted one on the key of an
    // existing object must leave that object alone — without the query
    // string, UploadPart would be a PUT of the object and Abort a DELETE.
    void multipartRequestsLeaveAnExistingObjectAlone()
    {
        const QByteArray victimData = "victim — must survive\n";
        const QString k = key(QStringLiteral("victim.txt"));
        const Result<PutObjectResult> victim = put(k, victimData);
        VERIFY_OK(victim);

        CreateMultipartUploadParams cp;
        cp.bucket = m_bucket;
        cp.key = k;
        const Result<CreateMultipartUploadResult> created =
            await<CreateMultipartUploadResult>([&](auto cb) { m_session->createMultipartUpload(cp, &m_ctx, std::move(cb)); });
        VERIFY_OK(created);

        const QByteArray partData = QByteArray("replacement part\n").repeated(64);
        const QString path = writeFile(QStringLiteral("victim-part.bin"), partData);
        UploadPartParams up;
        up.bucket = m_bucket;
        up.key = k;
        up.uploadId = created->uploadId;
        up.partNumber = 1;
        up.file = bodyFor(path, partData, 0, partData.size());
        VERIFY_OK((await<UploadPartResult>([&](auto cb) { m_session->uploadPart(up, &m_ctx, std::move(cb)); })));

        const auto victimUnchanged = [&] {
            const Result<ObjectData> got = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
            return got && got->data == victimData && got->meta.etag == victim->etag;
        };
        QVERIFY2(victimUnchanged(), "UploadPart changed the existing object");

        ListMultipartUploadsParams lp;
        lp.bucket = m_bucket;
        lp.prefix = k;
        const Result<ListMultipartUploadsPage> listed = await<ListMultipartUploadsPage>(
            [&](auto cb) { m_session->listMultipartUploads(lp, &m_ctx, std::move(cb)); });
        VERIFY_OK(listed);
        bool found = false;
        for (const MultipartUploadInfo& u : listed->uploads)
            found |= u.uploadId == created->uploadId && u.key == k;
        QVERIFY(found);

        VERIFY_OK((await<void>([&](auto cb) {
            m_session->abortMultipartUpload(m_bucket, k, created->uploadId, &m_ctx, std::move(cb));
        })));
        QVERIFY2(victimUnchanged(), "AbortMultipartUpload changed or deleted the existing object");
        const Result<ListMultipartUploadsPage> after = await<ListMultipartUploadsPage>(
            [&](auto cb) { m_session->listMultipartUploads(lp, &m_ctx, std::move(cb)); });
        VERIFY_OK(after);
        for (const MultipartUploadInfo& u : after->uploads)
            QVERIFY(u.uploadId != created->uploadId);
    }

    // If-None-Match: * on PutObject. it-expectations say "conditionalWrites":
    // true (refused with 412) or false (the backend ignores it and overwrites).
    void conditionalPutBehaviour()
    {
        const QString k = key(QStringLiteral("conditional.txt"));
        VERIFY_OK(put(k, "first"));
        const QByteArray second = "second";
        const QString path = writeFile(QStringLiteral("conditional.bin"), second);
        PutObjectParams p;
        p.bucket = m_bucket;
        p.key = k;
        p.file = bodyFor(path, second, 0, second.size());
        p.ifNoneMatch = true;
        const Result<PutObjectResult> r = await<PutObjectResult>([&](auto cb) { m_session->putObject(p, &m_ctx, std::move(cb)); });
        const Result<ObjectData> now = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(now);
        const bool honoured = !r && r.error().category == ErrorCategory::PreconditionFailed && now->data == "first";
        qInfo() << "If-None-Match on PutObject:" << (r ? QStringLiteral("accepted, object now \"%1\"").arg(QString::fromLatin1(now->data))
                                                       : r.error().toString());
        if (!r)
            QCOMPARE(now->data, QByteArray("first"));   // a refusal never writes
        const QJsonValue expected = m_expect.value(QLatin1String("conditionalWrites"));
        if (expected.isBool())
            QCOMPARE(honoured, expected.toBool());
    }

    // Opt-in: QBUCKET_IT_SECOND_BUCKET (another qbucket-it-* bucket on the
    // same account). The copy lands under this run's prefix there and is
    // removed again. it-expectations may say "crossBucketCopy": true/false.
    void crossBucketCopy()
    {
        const QString second = env("QBUCKET_IT_SECOND_BUCKET");
        if (second.isEmpty())
            QSKIP("QBUCKET_IT_SECOND_BUCKET not set");
        static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
        QVERIFY2(safeName.match(second).hasMatch() && second != m_bucket,
                 "refusing to run: QBUCKET_IT_SECOND_BUCKET must be another qbucket-it-* bucket");
        const QString k = key(QStringLiteral("cross-bucket.txt"));
        const Result<PutObjectResult> put = this->put(k, "across");
        VERIFY_OK(put);
        CopyObjectParams p;
        p.sourceBucket = m_bucket;
        p.sourceKey = k;
        p.bucket = second;
        p.key = k;
        p.ifMatch = put->etag;
        const Result<CopyObjectResult> r =
            await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
        qInfo().noquote() << "cross-bucket copy" << m_bucket << "->" << second << ":"
                          << (r ? QStringLiteral("works") : r.error().toString());
        if (r) {
            const Result<ObjectData> back = await<ObjectData>([&](auto cb) { m_session->getObject(second, k, {}, &m_ctx, std::move(cb)); });
            VERIFY_OK(back);
            QCOMPARE(back->data, QByteArray("across"));
            QVERIFY(k.startsWith(m_prefix));
            VERIFY_OK((await<DeleteObjectResult>([&](auto cb) { m_session->deleteObject(second, k, {}, &m_ctx, std::move(cb)); })));
        }
        const QJsonValue expected = m_expect.value(QLatin1String("crossBucketCopy"));
        if (expected.isBool())
            QCOMPARE(bool(r), expected.toBool());
    }

    void nastyKeysRoundTrip()
    {
        const QString base = key(QStringLiteral("nasty/"));
        QStringList expected;
        for (const QString& name : nastyKeys()) {
            if (expects("collapsesRepeatedSlashes") && name.contains(QLatin1String("//")))
                continue;   // asserted in repeatedSlashesCollapse()
            const QString k = base + name;
            const Result<PutObjectResult> r = put(k, name.toUtf8());
            QVERIFY2(r, qPrintable(name + u": " + (r ? QString() : r.error().toString())));
            expected.append(k);
        }
        // Listing (encoding-type=url) gives back exactly the keys we stored.
        Result<QStringList> listed = listKeys(base);
        VERIFY_OK(listed);
        QStringList got = listed.value();
        got.sort();
        expected.sort();
        QCOMPARE(got, expected);

        // Folder view.
        ListObjectsParams p;
        p.bucket = m_bucket;
        p.prefix = base;
        p.delimiter = QStringLiteral("/");
        const Result<ListPage> folder = await<ListPage>([&](auto cb) { m_session->listObjects(p, &m_ctx, std::move(cb)); });
        VERIFY_OK(folder);
        QVERIFY(folder->commonPrefixes.contains(base + QStringLiteral("dir/")));

        // Each key addresses its own object.
        for (const QString& k : expected) {
            const Result<ObjectData> d = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
            QVERIFY2(d, qPrintable(k + u": " + (d ? QString() : d.error().toString())));
            QCOMPARE(QString::fromUtf8(d->data), k.mid(base.size()));
        }

        // One batch delete for all of them.
        QList<ObjectIdentifier> ids;
        for (const QString& k : expected)
            ids.append({k, {}});
        const Result<DeleteObjectsResult> del = await<DeleteObjectsResult>([&](auto cb) { m_session->deleteObjects(m_bucket, ids, &m_ctx, std::move(cb)); });
        VERIFY_OK(del);
        for (const auto& f : del->failed)
            qWarning().noquote() << "not deleted:" << f.key << f.code << f.message;
        QCOMPARE(del->failed.size(), 0);
        listed = listKeys(base);
        VERIFY_OK(listed);
        QVERIFY(listed->isEmpty());
    }

    void repeatedSlashesCollapse()
    {
        if (!expects("collapsesRepeatedSlashes"))
            QSKIP("backend keeps repeated slashes (covered by nastyKeysRoundTrip)");
        // Documented deviation: 'a//b' lands on 'a/b'. Asserted so a fix shows up.
        const QString k = key(QStringLiteral("slashes//x.txt"));
        VERIFY_OK(put(k, "x"));
        const Result<QStringList> listed = listKeys(key(QStringLiteral("slashes/")));
        VERIFY_OK(listed);
        QCOMPARE(listed.value(), QStringList{key(QStringLiteral("slashes/x.txt"))});
        deleteAllUnder(key(QStringLiteral("slashes/")));
    }

    void controlCharacterKey()
    {
        // Listable thanks to encoding-type=url; not expressible in a batch
        // delete body, so it is deleted on its own.
        const QString k = key(QStringLiteral("ctrl\x01" "char.txt"));
        const Result<PutObjectResult> r = put(k, "x");
        if (!r && r.error().category == ErrorCategory::InvalidRequest)
            QSKIP("backend rejects control characters in keys");
        VERIFY_OK(r);
        const Result<QStringList> listed = listKeys(key(QStringLiteral("ctrl")));
        VERIFY_OK(listed);
        if (!expects("controlCharactersInKeys") && m_expect.contains(QLatin1String("controlCharactersInKeys"))) {
            // Documented deviation: the key does not survive.
            QVERIFY(listed.value() != QStringList{k});
            deleteAllUnder(key(QStringLiteral("ctrl")));
            return;
        }
        QCOMPARE(listed.value(), QStringList{k});
        VERIFY_OK((await<DeleteObjectResult>([&](auto cb) { m_session->deleteObject(m_bucket, k, {}, &m_ctx, std::move(cb)); })));
    }

    void pagingWithSmallPages()
    {
        const QString base = key(QStringLiteral("paging/"));
        for (int i = 0; i < 5; ++i)
            VERIFY_OK(put(base + QStringLiteral("k%1").arg(i), "p"));
        const Result<QStringList> keys = listKeys(base, 2);
        VERIFY_OK(keys);
        QCOMPARE(keys->size(), 5);
        deleteAllUnder(base);
    }

    void batchDeleteOfMissingKeysSucceeds()
    {
        const Result<DeleteObjectsResult> r = await<DeleteObjectsResult>([&](auto cb) {
            m_session->deleteObjects(m_bucket, {{key(QStringLiteral("never-existed")), {}}}, &m_ctx, std::move(cb));
        });
        VERIFY_OK(r);
        QCOMPARE(r->failed.size(), 0);
    }

    void presignedGet()
    {
        const QString k = key(QStringLiteral("presigned file.txt"));
        VERIFY_OK(put(k, "shared"));
        PresignRequest req;
        req.bucket = m_bucket;
        req.key = k;
        req.expiresSeconds = 300;
        const Result<QUrl> url = await<QUrl>([&](auto cb) { m_session->presign(req, &m_ctx, std::move(cb)); });
        VERIFY_OK(url);
        QNetworkAccessManager nam;
        std::unique_ptr<QNetworkReply> reply(nam.get(QNetworkRequest(url.value())));
        QSignalSpy done(reply.get(), &QNetworkReply::finished);
        QVERIFY(done.wait(30000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(reply->readAll(), QByteArray("shared"));
    }

    void wrongSecretIsAuthenticationError()
    {
        SessionOptions o = optionsWith(env("QBUCKET_IT_ACCESS_KEY"), QStringLiteral("definitely-wrong"));
        Session wrong(o);
        const Result<BucketList> r = await<BucketList>([&](auto cb) { wrong.listBuckets(&m_ctx, std::move(cb)); });
        QVERIFY(!r);
        const QString expected = m_expect.value(QLatin1String("wrongSecretCategory"))
                                     .toString(QStringLiteral("Authentication"));
        QCOMPARE(Error::categoryName(r.error().category), expected);
    }

    void clockSkewIsCorrected()
    {
        SessionOptions o = m_options;
        o.initialClockOffsetSecs = -qint64(20) * 60;   // pretend our clock is 20 min ahead
        Session skewed(o);
        QSignalSpy skew(&skewed, &Session::clockSkewDetected);
        const Result<BucketList> r = await<BucketList>([&](auto cb) { skewed.listBuckets(&m_ctx, std::move(cb)); });
        VERIFY_OK(r);
        QVERIFY(skew.size() >= 1);
    }

    void capabilitiesMatchExpectations()
    {
        // An object first, so the object-level probes have something to ask about.
        const QString target = key(QStringLiteral("probe-target.txt"));
        VERIFY_OK(put(target, "probe"));
        const Result<CapabilitySet> set = await<CapabilitySet>([&](auto cb) { m_session->probeBucket(m_bucket, target, &m_ctx, std::move(cb)); });
        VERIFY_OK(set);
        QStringList report;
        for (const auto& [f, e] : set->entries())
            report.append(QStringLiteral("\"%1\": \"%2\"").arg(featureId(f), stateName(e.state)));
        qInfo().noquote() << "probe results:" << report.join(QStringLiteral(", "));

        const QJsonObject state = m_expect.value(QLatin1String("bucketState")).toObject();
        if (state.contains(QLatin1String("versioning")))
            QCOMPARE(set->bucketState.versioning, state.value(QLatin1String("versioning")).toString());
        if (state.contains(QLatin1String("objectLockEnabled")))
            QCOMPARE(set->bucketState.objectLockEnabled,
                     std::optional(state.value(QLatin1String("objectLockEnabled")).toBool()));

        const QJsonObject expected = m_expect.value(QLatin1String("capabilities")).toObject();
        if (expected.isEmpty())
            QSKIP("no capability expectations for this backend yet");
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            const std::optional<Feature> f = featureFromId(it.key());
            QVERIFY2(f, qPrintable(u"unknown feature id " + it.key()));
            QCOMPARE(QStringLiteral("%1=%2").arg(it.key(), stateName(set->state(*f))),
                     QStringLiteral("%1=%2").arg(it.key(), it.value().toString()));
        }
        deleteAllUnder(key(QStringLiteral("probe-target")));
    }

    void restrictedUserIsNotPermitted()
    {
        if (env("QBUCKET_IT_RESTRICTED_ACCESS_KEY").isEmpty())
            QSKIP("no restricted user configured");
        Session restricted(optionsWith(env("QBUCKET_IT_RESTRICTED_ACCESS_KEY"),
                                       env("QBUCKET_IT_RESTRICTED_SECRET_KEY")));
        ListObjectsParams p;
        p.bucket = m_bucket;
        const Result<ListPage> r = await<ListPage>([&](auto cb) { restricted.listObjects(p, &m_ctx, std::move(cb)); });
        QVERIFY(!r);
        QCOMPARE(r.error().category, ErrorCategory::NotPermitted);
    }
};

QTEST_GUILESS_MAIN(ItS3Backend)
#include "it_s3_backend.moc"
