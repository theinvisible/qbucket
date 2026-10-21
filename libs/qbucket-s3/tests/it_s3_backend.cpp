// Integration tests against a real S3 endpoint — opt-in, skipped unless
// QBUCKET_IT_ENDPOINT is set (PLAN.md §7):
//
//   QBUCKET_IT_ENDPOINT, QBUCKET_IT_REGION, QBUCKET_IT_ACCESS_KEY,
//   QBUCKET_IT_SECRET_KEY, QBUCKET_IT_BUCKET (must match qbucket-it-*),
//   QBUCKET_IT_ADDRESSING (path|virtual), QBUCKET_IT_PROVIDER, QBUCKET_IT_BACKEND,
//   optional: QBUCKET_IT_CREATE_BUCKET=1 (local backends only; with
//             "bucketVersioningWrite" expected, <bucket>-versions is created
//             and versioned for the version tests),
//             QBUCKET_IT_RESTRICTED_ACCESS_KEY / _SECRET_KEY (a user without
//             access to the bucket, for NotPermitted),
//             QBUCKET_IT_VERSIONING_BUCKET (a qbucket-it-* scratch bucket
//             holding guard/DO-NOT-DELETE.txt: toggleVersioning switches its
//             versioning off and on again),
//             QBUCKET_IT_RUN (16 hex digits: reuse an earlier run's prefix,
//             e.g. to clean up after it)
//
// Known deviations of a backend live in tests/data/it-expectations/
// <backend>.json. They are asserted, not just skipped — when a backend
// changes, the test notices.
//
// Safety: refuses any bucket not named qbucket-it-*; every key lives under
// it/<run id>/ and only that prefix is cleaned up — every version and delete
// marker under it, nothing else. The bucket itself is never deleted.

#include <qbucket/s3/ListObjectVersionsPager.h>
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
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <qbucket/s3/RequestObserver.h>

#include <QtTest>

#include <optional>

using namespace qbucket::s3;

namespace {

QString env(const char* name)
{
    return qEnvironmentVariable(name);
}

bool safeBucketName(const QString& bucket)
{
    static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
    return safeName.match(bucket).hasMatch();
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
    QString m_versionBucket;        // versioning enabled; see versionedBucket()
    bool m_versionBucketResolved = false;
    bool m_versionBucketCreated = false;
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

    Result<PutObjectResult> put(const QString& k, const QByteArray& data) { return putIn(m_bucket, k, data); }

    Result<PutObjectResult> putIn(const QString& bucket, const QString& k, const QByteArray& data)
    {
        PutObjectParams p;
        p.bucket = bucket;
        p.key = k;
        p.data = data;
        p.contentType = QStringLiteral("text/plain");
        return await<PutObjectResult>([&](auto cb) { m_session->putObject(p, &m_ctx, std::move(cb)); });
    }

    // Every key under `prefix`, walking all pages.
    Result<QStringList> listKeys(const QString& prefix, int maxKeys = 1000, const QString& bucket = {})
    {
        ListObjectsParams p;
        p.bucket = bucket.isEmpty() ? m_bucket : bucket;
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

    // Every version and delete marker under `prefix`, walking all pages.
    Result<QList<VersionEntry>> listVersions(const QString& bucket, const QString& prefix, int maxKeys = 1000,
                                             const QString& delimiter = {}, QStringList* prefixes = nullptr)
    {
        ListObjectVersionsParams p;
        p.bucket = bucket;
        p.prefix = prefix;
        p.delimiter = delimiter;
        p.maxKeys = maxKeys;
        ListObjectVersionsPager pager(*m_session, p);
        QList<VersionEntry> out;
        while (pager.hasMore()) {
            Result<ListObjectVersionsPage> page =
                await<ListObjectVersionsPage>([&](auto cb) { pager.fetchNext(&m_ctx, std::move(cb)); });
            if (!page)
                return page.error();
            out += page->entries;
            if (prefixes)
                *prefixes += page->commonPrefixes;
        }
        return out;
    }

    void deleteBatch(const QString& bucket, const QList<ObjectIdentifier>& batch)
    {
        const Result<DeleteObjectsResult> r = await<DeleteObjectsResult>(
            [&](auto cb) { m_session->deleteObjects(bucket, batch, &m_ctx, std::move(cb)); });
        if (!r && r.error().category == ErrorCategory::InvalidRequest) {
            // A key XML can't carry (control characters) refuses the whole
            // batch: one by one instead.
            for (const ObjectIdentifier& o : batch) {
                const Result<DeleteObjectResult> one = await<DeleteObjectResult>(
                    [&](auto cb) { m_session->deleteObject(bucket, o.key, o.versionId, &m_ctx, std::move(cb)); });
                if (!one)
                    qWarning().noquote() << "cleanup left" << o.key << o.versionId << one.error().toString();
            }
            return;
        }
        if (!r) {
            qWarning().noquote() << "cleanup:" << r.error().toString();
            return;
        }
        for (const DeleteObjectsResult::Failed& f : r->failed)
            qWarning().noquote() << "cleanup left" << f.key << f.versionId << f.code;   // e.g. object lock
    }

    // Removes everything under `prefix` for good: every version and delete
    // marker where versions can be listed, else the current objects. Never
    // touches anything outside this run's prefix.
    void deleteAllUnder(const QString& prefix, const QString& bucketName = {})
    {
        const QString bucket = bucketName.isEmpty() ? m_bucket : bucketName;
        if (!prefix.startsWith(m_prefix))
            return;
        QList<ObjectIdentifier> all;
        if (const Result<QList<VersionEntry>> versions = listVersions(bucket, prefix)) {
            for (const VersionEntry& v : versions.value()) {
                if (v.key.startsWith(m_prefix))
                    all.append({v.key, v.versionId});
            }
        } else if (const Result<QStringList> keys = listKeys(prefix, 1000, bucket)) {   // no version listing (Garage)
            for (const QString& k : keys.value()) {
                if (k.startsWith(m_prefix))
                    all.append({k, {}});
            }
        } else {
            return;
        }
        for (qsizetype i = 0; i < all.size(); i += 1000)
            deleteBatch(bucket, all.mid(i, 1000));
    }

    std::optional<VersioningStatus> versioningOf(const QString& bucket)
    {
        const Result<BucketVersioning> v =
            await<BucketVersioning>([&](auto cb) { m_session->getBucketVersioning(bucket, &m_ctx, std::move(cb)); });
        if (!v)
            return std::nullopt;
        return v->status;
    }

    Result<SubresourceWriteResult> setVersioning(const QString& bucket, VersioningStatus status)
    {
        PutBucketVersioningParams p;
        p.bucket = bucket;
        p.status = status;
        return await<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_ctx, std::move(cb)); });
    }

    // Reads the versioning back until it shows `want` (gateways may answer
    // stale for a while after a change). Returns the seconds it took, or -1.
    int waitForVersioning(const QString& bucket, VersioningStatus want, int maxSecs = 120)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < qint64(maxSecs) * 1000) {
            if (versioningOf(bucket) == want)
                return int(t.elapsed() / 1000);
            QTest::qWait(3000);
        }
        return -1;
    }

    // A bucket with versioning enabled for the version tests: the test
    // bucket itself when it is versioned, or — on local backends that can
    // switch versioning on — <bucket>-versions, created here. Empty: none.
    QString versionedBucket()
    {
        if (m_versionBucketResolved)
            return m_versionBucket;
        m_versionBucketResolved = true;
        if (versioningOf(m_bucket) == VersioningStatus::Enabled) {
            m_versionBucket = m_bucket;
        } else if (env("QBUCKET_IT_CREATE_BUCKET") == QLatin1String("1") && expects("bucketVersioningWrite")) {
            const QString bucket = m_bucket + QStringLiteral("-versions");
            CreateBucketParams p;
            p.bucket = bucket;
            p.region = m_options.endpoint.region;
            const Result<void> created = await<void>([&](auto cb) { m_session->createBucket(p, &m_ctx, std::move(cb)); });
            if ((created || created.error().category == ErrorCategory::Conflict)
                && setVersioning(bucket, VersioningStatus::Enabled)
                && waitForVersioning(bucket, VersioningStatus::Enabled, 30) >= 0) {
                m_versionBucket = bucket;
                m_versionBucketCreated = true;
            }
        }
        qInfo().noquote() << "versioned bucket:" << (m_versionBucket.isEmpty() ? QStringLiteral("none") : m_versionBucket);
        return m_versionBucket;
    }

    // The current object's version id (empty when there is none).
    QString currentVersion(const QString& bucket, const QString& k)
    {
        const Result<ObjectMeta> m =
            await<ObjectMeta>([&](auto cb) { m_session->headObject(bucket, k, {}, &m_ctx, std::move(cb)); });
        return m ? m->versionId : QString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (env("QBUCKET_IT_ENDPOINT").isEmpty())
            QSKIP("QBUCKET_IT_ENDPOINT not set — integration tests are opt-in");

        m_bucket = env("QBUCKET_IT_BUCKET");
        QVERIFY2(safeBucketName(m_bucket), "refusing to run: QBUCKET_IT_BUCKET must be a dedicated qbucket-it-* bucket");
        if (!env("QBUCKET_IT_VERSIONING_BUCKET").isEmpty()) {
            QVERIFY2(safeBucketName(env("QBUCKET_IT_VERSIONING_BUCKET")),
                     "refusing to run: QBUCKET_IT_VERSIONING_BUCKET must be a dedicated qbucket-it-* bucket");
        }
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
        // QBUCKET_IT_TRACE_BODIES=1: print every response body (diagnosing a
        // backend's odd answer; signatures are redacted in the URL).
        if (qEnvironmentVariableIntValue("QBUCKET_IT_TRACE_BODIES") == 1) {
            struct Trace final : RequestObserver {
                void onRequestFinished(const RequestRecord& r) override
                {
                    qInfo().noquote() << r.operation << r.httpStatus << r.url << "\n   " << QString::fromUtf8(r.responseBody.left(800));
                }
            };
            m_options.observer = std::make_shared<Trace>();
            m_options.captureResponseBodyBytes = 4096;
        }
        m_options = optionsWith(env("QBUCKET_IT_ACCESS_KEY"), env("QBUCKET_IT_SECRET_KEY"));
        QVERIFY2(m_options.endpoint.isValid(), qPrintable(m_options.endpoint.validationError()));
        m_session = std::make_unique<Session>(m_options);

        // QBUCKET_IT_RUN repeats an earlier run's prefix — to clean up after
        // it: `it_s3_backend bucketIsVisible` (any one test; the cleanup runs).
        static const QRegularExpression runId(QStringLiteral("^[0-9a-f]{16}$"));
        if (!env("QBUCKET_IT_RUN").isEmpty()) {
            QVERIFY2(runId.match(env("QBUCKET_IT_RUN")).hasMatch(), "QBUCKET_IT_RUN must be 16 hex digits");
            m_prefix = QStringLiteral("it/%1/").arg(env("QBUCKET_IT_RUN"));
        } else {
            const quint64 run = QRandomGenerator::global()->generate64();
            m_prefix = QStringLiteral("it/%1/").arg(run, 16, 16, QLatin1Char('0'));
        }
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
            if (!m_versionBucket.isEmpty() && m_versionBucket != m_bucket)
                deleteAllUnder(m_prefix, m_versionBucket);
            const QString second = env("QBUCKET_IT_SECOND_BUCKET");   // crossBucketCopy's target
            if (safeBucketName(second) && second != m_bucket)
                deleteAllUnder(m_prefix, second);
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

    // Editing headers / metadata = a copy onto itself with REPLACE. The
    // content stays, every header is exactly what was sent (an unsent one is
    // gone), a COPY to another key carries them, a stale if-match changes
    // nothing. it-expectations "replaceDropsUnsentHeaders" (default true):
    // VersityGW keeps standard headers that a REPLACE leaves out.
    void copyInPlaceReplacesMetadata()
    {
        const QByteArray data = QByteArray("edit me\n").repeated(300);
        const QString k = key(QStringLiteral("edit/notes.md"));
        const Result<PutObjectResult> put = this->put(k, data);
        VERIFY_OK(put);
        const auto head = [&](const QString& which) {
            return await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, which, {}, &m_ctx, std::move(cb)); });
        };

        CopyObjectParams p;
        p.sourceBucket = p.bucket = m_bucket;
        p.sourceKey = p.key = k;
        p.ifMatch = put->etag;
        p.metadataDirective = MetadataDirective::Replace;
        p.headers.contentType = QStringLiteral("text/markdown");
        p.headers.cacheControl = QStringLiteral("max-age=60");
        p.headers.contentDisposition = QStringLiteral("attachment; filename=\"notes.md\"");
        p.headers.contentLanguage = QStringLiteral("de");
        p.headers.userMetadata.insert(QStringLiteral("stage"), QStringLiteral("edited"));
        const Result<CopyObjectResult> edited =
            await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
        VERIFY_OK(edited);
        Result<ObjectMeta> m = head(k);
        VERIFY_OK(m);
        qInfo().noquote() << "after REPLACE:" << m->contentType << "|" << m->cacheControl << "|" << m->contentDisposition
                          << "|" << m->contentLanguage << "| meta" << m->userMetadata << "| etag" << m->etag
                          << "(was" << put->etag << ")";
        QCOMPARE(m->size, qint64(data.size()));
        QCOMPARE(objectHeadersOf(*m), p.headers);   // setting works everywhere

        // Removing a header = not sending it.
        p.ifMatch = m->etag;
        p.headers.cacheControl.clear();
        p.headers.userMetadata.clear();
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); })));
        m = head(k);
        VERIFY_OK(m);
        qInfo().noquote() << "after removing:" << m->contentType << "|" << m->cacheControl << "|" << m->contentDisposition
                          << "|" << m->contentLanguage << "| meta" << m->userMetadata;
        const bool dropped = objectHeadersOf(*m) == p.headers;
        QVERIFY(m->userMetadata.isEmpty());   // user metadata is always replaced as a whole

        // COPY to another key carries the headers along.
        CopyObjectParams copy;
        copy.sourceBucket = copy.bucket = m_bucket;
        copy.sourceKey = k;
        copy.key = key(QStringLiteral("edit/notes copy.md"));
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(copy, &m_ctx, std::move(cb)); })));
        const Result<ObjectMeta> c = head(copy.key);
        VERIFY_OK(c);
        QCOMPARE(objectHeadersOf(*c), objectHeadersOf(*m));

        // A stale if-match: refused, nothing changed.
        p.ifMatch = QStringLiteral("\"00000000000000000000000000000000\"");
        p.headers.contentType = QStringLiteral("application/x-never");
        const Result<CopyObjectResult> stale =
            await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
        QVERIFY(!stale);
        QCOMPARE(stale.error().category, ErrorCategory::PreconditionFailed);
        const Result<ObjectData> after = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(after);
        QCOMPARE(after->data, data);   // the content survived every edit
        QVERIFY(after->meta.contentType != QStringLiteral("application/x-never"));
        QCOMPARE(dropped, m_expect.value(QLatin1String("replaceDropsUnsentHeaders")).toBool(true));
    }

    // For the record: a copy onto itself that only names a storage class
    // (directive COPY) — allowed on AWS; what does the backend do with its
    // own class and with STANDARD_IA? Only the content is asserted.
    void copyInPlaceClassChange()
    {
        const QString k = key(QStringLiteral("class/obj.txt"));
        VERIFY_OK(put(k, "class"));
        for (const QString& cls : {QStringLiteral("STANDARD"), QStringLiteral("STANDARD_IA")}) {
            CopyObjectParams p;
            p.sourceBucket = p.bucket = m_bucket;
            p.sourceKey = p.key = k;
            p.storageClass = cls;
            const Result<CopyObjectResult> r =
                await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
            const Result<ObjectData> now = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
            VERIFY_OK(now);
            qInfo().noquote() << "copy in place, COPY directive, class" << cls << ":"
                              << (r ? QStringLiteral("ok, class now \"%1\"").arg(now->meta.storageClass) : r.error().toString())
                              << "| content-type" << now->meta.contentType;
            QCOMPARE(now->data, QByteArray("class"));
        }
    }

    // --- tags and ACLs: write support is never assumed from a read probe ---

    Result<SubresourceWriteResult> putTags(const QString& k, const TagSet& tags)
    {
        PutObjectTaggingParams p;
        p.bucket = m_bucket;
        p.key = k;
        p.tags = tags;
        return await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectTagging(p, &m_ctx, std::move(cb)); });
    }

    Result<SubresourceWriteResult> putAcl(const QString& k, const QString& canned)
    {
        PutObjectAclParams p;
        p.bucket = m_bucket;
        p.key = k;
        p.cannedAcl = canned;
        return await<SubresourceWriteResult>([&](auto cb) { m_session->putObjectAcl(p, &m_ctx, std::move(cb)); });
    }

    std::optional<QString> cannedAclOfObject(const QString& k)
    {
        const Result<AccessControlPolicy> acl =
            await<AccessControlPolicy>([&](auto cb) { m_session->getObjectAcl(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        if (!acl)
            return QStringLiteral("<error: %1>").arg(acl.error().toString());
        return cannedAclOf(*acl);
    }

    // The canary — run first: tag / ACL writes on a key that doesn't exist
    // must fail and create nothing. An object appearing means the server (or
    // a proxy) dropped the subresource and stored the request as an upload.
    void subresourceWritesOnMissingKeyCreateNothing()
    {
        const QString k = key(QStringLiteral("canary/never-created.txt"));
        const Result<SubresourceWriteResult> tags = putTags(k, {{QStringLiteral("canary"), QStringLiteral("1")}});
        const Result<SubresourceWriteResult> acl = putAcl(k, QStringLiteral("private"));
        qInfo().noquote() << "tag write on a missing key:" << (tags ? QStringLiteral("accepted!") : tags.error().toString());
        qInfo().noquote() << "ACL write on a missing key:" << (acl ? QStringLiteral("accepted!") : acl.error().toString());
        const Result<ObjectMeta> head =
            await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        QVERIFY2(!head, "an object appeared: the server stored a subresource write as an upload");
        QCOMPARE(head.error().category, ErrorCategory::NotFound);
        QVERIFY(!tags);
        QVERIFY(!acl);
        QVERIFY(tags.error().category != ErrorCategory::Protocol && acl.error().category != ErrorCategory::Protocol);
    }

    // The victim: an existing object keeps its content, ETag and size through
    // tag and ACL writes, and the writes read back. it-expectations
    // "objectTaggingWrite" / "objectAclWrite" say whether they work at all.
    void objectSubresourceWritesLeaveDataAlone()
    {
        const QByteArray data = QByteArray("victim\n").repeated(200);
        const QString k = key(QStringLiteral("victim/precious.txt"));
        const Result<PutObjectResult> put = this->put(k, data);
        VERIFY_OK(put);
        const auto unchanged = [&](const char* step) {
            const Result<ObjectData> now = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
            if (!now || now->data != data || now->meta.etag != put->etag) {
                qWarning() << "after" << step << ": object changed or gone —" << (now ? now->meta.etag : now.error().toString());
                return false;
            }
            return true;
        };

        const TagSet set{{QStringLiteral("project"), QStringLiteral("qbucket")}, {QStringLiteral("check"), QStringLiteral("victim")}};
        const Result<SubresourceWriteResult> tagged = putTags(k, set);
        qInfo().noquote() << "tag write:" << (tagged ? QStringLiteral("ok") : tagged.error().toString());
        QVERIFY(unchanged("the tag write"));
        if (tagged) {
            const Result<ObjectTagging> back =
                await<ObjectTagging>([&](auto cb) { m_session->getObjectTagging(m_bucket, k, {}, &m_ctx, std::move(cb)); });
            VERIFY_OK(back);
            QCOMPARE(back->tags.size(), 2);
            VERIFY_OK(putTags(k, {}));
            QVERIFY(unchanged("removing the tags"));
            const Result<ObjectTagging> none =
                await<ObjectTagging>([&](auto cb) { m_session->getObjectTagging(m_bucket, k, {}, &m_ctx, std::move(cb)); });
            VERIFY_OK(none);
            QVERIFY(none->tags.isEmpty());
        } else {
            QVERIFY2(tagged.error().category != ErrorCategory::Protocol, "answered like an upload");
        }

        const Result<SubresourceWriteResult> pub = putAcl(k, QStringLiteral("public-read"));
        qInfo().noquote() << "ACL write:" << (pub ? QStringLiteral("ok") : pub.error().toString());
        QVERIFY(unchanged("the ACL write"));
        if (pub) {
            QCOMPARE(cannedAclOfObject(k), std::optional<QString>(QStringLiteral("public-read")));
            VERIFY_OK(putAcl(k, QStringLiteral("private")));
            QVERIFY(unchanged("the second ACL write"));
            QCOMPARE(cannedAclOfObject(k), std::optional<QString>(QStringLiteral("private")));
        } else {
            QVERIFY2(pub.error().category != ErrorCategory::Protocol, "answered like an upload");
        }

        const QJsonValue tagsExpected = m_expect.value(QLatin1String("objectTaggingWrite"));
        if (tagsExpected.isBool())
            QCOMPARE(bool(tagged), tagsExpected.toBool());
        const QJsonValue aclExpected = m_expect.value(QLatin1String("objectAclWrite"));
        if (aclExpected.isBool())
            QCOMPARE(bool(pub), aclExpected.toBool());
    }

    // Tags travel with a COPY (also an in-place REPLACE of the metadata —
    // the tagging directive stays COPY). it-expectations "copyKeepsTags".
    void copyKeepsTags()
    {
        if (!m_expect.value(QLatin1String("objectTaggingWrite")).toBool(true))
            QSKIP("no object tags on this backend");
        const QString src = key(QStringLiteral("tags/src.txt"));
        VERIFY_OK(put(src, "tagged"));
        const TagSet set{{QStringLiteral("keep"), QStringLiteral("me")}};
        VERIFY_OK(putTags(src, set));
        CopyObjectParams p;
        p.sourceBucket = p.bucket = m_bucket;
        p.sourceKey = src;
        p.key = key(QStringLiteral("tags/dst.txt"));
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); })));
        const auto tagsOf = [&](const QString& k) {
            return await<ObjectTagging>([&](auto cb) { m_session->getObjectTagging(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        };
        const Result<ObjectTagging> copied = tagsOf(p.key);
        VERIFY_OK(copied);
        CopyObjectParams edit;
        edit.sourceBucket = edit.bucket = m_bucket;
        edit.sourceKey = edit.key = src;
        edit.metadataDirective = MetadataDirective::Replace;
        edit.headers.contentType = QStringLiteral("text/x-edited");
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(edit, &m_ctx, std::move(cb)); })));
        const Result<ObjectTagging> edited = tagsOf(src);
        VERIFY_OK(edited);
        // Sending the tags along (tagging directive REPLACE) keeps them everywhere.
        VERIFY_OK(putTags(src, set));
        edit.tags = set;
        edit.headers.contentType = QStringLiteral("text/x-edited-again");
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(edit, &m_ctx, std::move(cb)); })));
        const Result<ObjectTagging> resent = tagsOf(src);
        const Result<ObjectMeta> resentHead =
            await<ObjectMeta>([&](auto cb) { m_session->headObject(m_bucket, src, {}, &m_ctx, std::move(cb)); });
        qInfo().noquote() << "tags after COPY:" << copied->tags.size() << "| in-place REPLACE (tagging directive COPY):"
                          << edited->tags.size() << "| with the tags sent along:"
                          << (resent ? QString::number(resent->tags.size()) : resent.error().toString())
                          << "| HEAD tag count" << (resentHead ? resentHead->tagCount : -99);
        const bool replaceWorks = resent && resent->tags == set;
        // Edit, then write the tags again: what the app does where a copy loses them.
        VERIFY_OK(putTags(src, set));
        const Result<ObjectTagging> restored = tagsOf(src);
        VERIFY_OK(restored);
        QCOMPARE(restored->tags, set);
        const bool kept = copied->tags == set && edited->tags == set;
        QCOMPARE(kept, m_expect.value(QLatin1String("copyKeepsTags")).toBool(true));
        QCOMPARE(replaceWorks, m_expect.value(QLatin1String("copyReplacesTags")).toBool(true));
    }

    // A copy never carries the ACL: private unless x-amz-acl is sent.
    // it-expectations "copyResetsAcl".
    void copyResetsAclUnlessCanned()
    {
        if (!m_expect.value(QLatin1String("objectAclWrite")).toBool(true))
            QSKIP("no object ACLs on this backend");
        const QString src = key(QStringLiteral("acl/src.txt"));
        VERIFY_OK(put(src, "public for a moment"));
        VERIFY_OK(putAcl(src, QStringLiteral("public-read")));
        CopyObjectParams p;
        p.sourceBucket = p.bucket = m_bucket;
        p.sourceKey = src;
        p.key = key(QStringLiteral("acl/plain copy.txt"));
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); })));
        const std::optional<QString> plain = cannedAclOfObject(p.key);
        p.key = key(QStringLiteral("acl/canned copy.txt"));
        p.cannedAcl = QStringLiteral("public-read");
        VERIFY_OK((await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); })));
        const std::optional<QString> canned = cannedAclOfObject(p.key);
        VERIFY_OK(putAcl(src, QStringLiteral("private")));
        qInfo() << "ACL of a plain copy:" << plain.value_or(QStringLiteral("custom")) << "| with x-amz-acl:"
                << canned.value_or(QStringLiteral("custom"));
        QCOMPARE(canned, std::optional<QString>(QStringLiteral("public-read")));
        const bool reset = plain == std::optional<QString>(QStringLiteral("private"));
        QCOMPARE(reset, m_expect.value(QLatin1String("copyResetsAcl")).toBool(true));
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
        cp.headers.contentType = QStringLiteral("application/x-test");
        cp.headers.userMetadata.insert(QStringLiteral("origin"), QStringLiteral("it"));
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

    // An edit of an object above 5 GiB is a multipart copy onto itself with
    // replaced headers (here with 5 MiB parts): content intact, headers new.
    void multipartCopyInPlaceWithReplace()
    {
        const QByteArray data = QByteArray("edit in parts 0123456789\n").repeated(260000);   // 6.2 MB
        const QString k = key(QStringLiteral("partedit/obj.bin"));
        const Result<PutObjectResult> put = this->put(k, data);
        VERIFY_OK(put);
        transfer::CopyRequest r;
        r.sourceBucket = r.bucket = m_bucket;
        r.sourceKey = r.key = k;
        r.ifMatch = put->etag;
        r.partSize = Q_INT64_C(5) * 1024 * 1024;
        ObjectHeaders h;
        h.contentType = QStringLiteral("application/x-edited");
        h.cacheControl = QStringLiteral("max-age=120");
        h.userMetadata.insert(QStringLiteral("edited"), QStringLiteral("in-parts"));
        r.replaceHeaders = h;
        transfer::Copier copier(m_session.get(), r, nullptr);
        QSignalSpy finished(&copier, &transfer::Copier::finished);
        copier.start();
        QVERIFY(finished.wait(120000));
        VERIFY_OK(copier.result());
        QCOMPARE(copier.result()->parts, 2);
        const Result<ObjectData> got = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(got);
        QCOMPARE(got->data, data);
        QCOMPARE(objectHeadersOf(got->meta), h);
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

        // "Download as": the server answers with the requested disposition.
        req.responseContentDisposition = QStringLiteral("attachment; filename=\"shared.txt\"");
        const Result<QUrl> named = await<QUrl>([&](auto cb) { m_session->presign(req, &m_ctx, std::move(cb)); });
        VERIFY_OK(named);
        std::unique_ptr<QNetworkReply> second(nam.get(QNetworkRequest(named.value())));
        QSignalSpy secondDone(second.get(), &QNetworkReply::finished);
        QVERIFY(secondDone.wait(30000));
        QCOMPARE(second->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        qInfo() << "Content-Disposition of a named link:" << second->rawHeader("Content-Disposition");
        QCOMPARE(second->rawHeader("Content-Disposition"), QByteArray("attachment; filename=\"shared.txt\""));
    }

    // A presigned PUT uploads without credentials; the object reads back.
    void presignedPut()
    {
        const QString k = key(QStringLiteral("presigned upload.txt"));
        PresignRequest req;
        req.method = "PUT";
        req.bucket = m_bucket;
        req.key = k;
        req.expiresSeconds = 300;
        const Result<QUrl> url = await<QUrl>([&](auto cb) { m_session->presign(req, &m_ctx, std::move(cb)); });
        VERIFY_OK(url);
        QNetworkAccessManager nam;
        std::unique_ptr<QNetworkReply> reply(nam.put(QNetworkRequest(url.value()), QByteArray("uploaded via link")));
        QSignalSpy done(reply.get(), &QNetworkReply::finished);
        QVERIFY(done.wait(30000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        const Result<ObjectData> back = await<ObjectData>([&](auto cb) { m_session->getObject(m_bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(back);
        QCOMPARE(back->data, QByteArray("uploaded via link"));
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

    // --- versions ---------------------------------------------------------

    void listVersionsPagesAndMarkers()
    {
        const QString bucket = versionedBucket();
        if (bucket.isEmpty())
            QSKIP("no versioned bucket on this backend");
        const QString base = key(QStringLiteral("versions-list/"));
        QStringList multiIds;   // newest first
        for (int i = 0; i < 3; ++i) {
            const Result<PutObjectResult> r = putIn(bucket, base + QStringLiteral("multi"), QByteArray::number(i));
            VERIFY_OK(r);
            QVERIFY(!r->versionId.isEmpty() && r->versionId != QLatin1String("null"));
            multiIds.prepend(r->versionId);
        }
        VERIFY_OK(putIn(bucket, base + QStringLiteral("gone"), "x"));
        const Result<DeleteObjectResult> del = await<DeleteObjectResult>([&](auto cb) {
            m_session->deleteObject(bucket, base + QStringLiteral("gone"), {}, &m_ctx, std::move(cb));
        });
        VERIFY_OK(del);
        QVERIFY(del->deleteMarker);
        VERIFY_OK(putIn(bucket, base + QStringLiteral("sub dir/inner"), "y"));

        const Result<QList<VersionEntry>> flat = listVersions(bucket, base, 2);
        VERIFY_OK(flat);
        QCOMPARE(flat->size(), 6);   // 3 + version and marker + 1
        QStringList seen;
        for (const VersionEntry& v : flat.value()) {
            if (v.key == base + QLatin1String("multi"))
                seen.append(v.versionId);
            if (v.key == base + QLatin1String("gone") && v.isLatest) {
                QVERIFY(v.isDeleteMarker);
                QCOMPARE(v.versionId, del->versionId);
            }
        }
        QCOMPARE(seen, multiIds);   // newest first, across 2-entry pages

        QStringList prefixes;
        const Result<QList<VersionEntry>> folders = listVersions(bucket, base, 1000, QStringLiteral("/"), &prefixes);
        VERIFY_OK(folders);
        QCOMPARE(prefixes, QStringList{base + QStringLiteral("sub dir/")});
        QCOMPARE(folders->size(), 5);
        deleteAllUnder(base, bucket);
        const Result<QList<VersionEntry>> after = listVersions(bucket, base);
        VERIFY_OK(after);
        QVERIFY(after->isEmpty());
    }

    void deleteMarkerSemantics()
    {
        const QString bucket = versionedBucket();
        if (bucket.isEmpty())
            QSKIP("no versioned bucket on this backend");
        const QString k = key(QStringLiteral("versions-marker/doc.txt"));
        const Result<PutObjectResult> put = putIn(bucket, k, "keep me");
        VERIFY_OK(put);
        const Result<DeleteObjectResult> del = await<DeleteObjectResult>(
            [&](auto cb) { m_session->deleteObject(bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(del);
        QVERIFY(del->deleteMarker);
        const QString marker = del->versionId;
        QVERIFY(!marker.isEmpty());

        // AWS answers with x-amz-delete-marker; RGW and VersityGW with a bare
        // 404 — a marker is only certain from the version listing.
        const auto expectedOr = [&](const char* quirk) {
            return !m_expect.contains(QLatin1String(quirk)) || expects(quirk);
        };
        Result<ObjectMeta> head = await<ObjectMeta>([&](auto cb) { m_session->headObject(bucket, k, {}, &m_ctx, std::move(cb)); });
        QCOMPARE(head.error().category, ErrorCategory::NotFound);
        QCOMPARE(head.error().deleteMarker, expectedOr("headReportsDeleteMarker"));
        head = await<ObjectMeta>([&](auto cb) { m_session->headObject(bucket, k, marker, &m_ctx, std::move(cb)); });
        QVERIFY(!head);
        qInfo().noquote() << "HEAD on the marker version:" << head.error().toString();
        QCOMPARE(head.error().category, ErrorCategory::NotFound);
        QCOMPARE(head.error().deleteMarker, expectedOr("headOnMarkerVersionReportsDeleteMarker"));

        // Removing the marker brings the object back.
        const Result<DeleteObjectResult> undelete = await<DeleteObjectResult>(
            [&](auto cb) { m_session->deleteObject(bucket, k, marker, &m_ctx, std::move(cb)); });
        VERIFY_OK(undelete);
        QCOMPARE(currentVersion(bucket, k), put->versionId);
        deleteAllUnder(key(QStringLiteral("versions-marker/")), bucket);
    }

    void getAndCopyAVersion()
    {
        const QString bucket = versionedBucket();
        if (bucket.isEmpty())
            QSKIP("no versioned bucket on this backend");
        const QString k = key(QStringLiteral("versions-restore/doc.txt"));
        const Result<PutObjectResult> first = putIn(bucket, k, "first version");
        VERIFY_OK(first);
        VERIFY_OK(putIn(bucket, k, "second version"));

        const Result<ObjectData> old = await<ObjectData>(
            [&](auto cb) { m_session->getObject(bucket, k, first->versionId, &m_ctx, std::move(cb)); });
        VERIFY_OK(old);
        QCOMPARE(old->data, QByteArray("first version"));
        QCOMPARE(old->meta.versionId, first->versionId);

        // Restore: the old version copied over the current one.
        CopyObjectParams p;
        p.sourceBucket = p.bucket = bucket;
        p.sourceKey = p.key = k;
        p.sourceVersionId = first->versionId;
        const Result<CopyObjectResult> copied =
            await<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_ctx, std::move(cb)); });
        VERIFY_OK(copied);
        // RGW sends neither x-amz-copy-source-version-id nor x-amz-version-id.
        if (!m_expect.contains(QLatin1String("copyReportsVersionIds")) || expects("copyReportsVersionIds")) {
            QCOMPARE(copied->sourceVersionId, first->versionId);
            QVERIFY(!copied->versionId.isEmpty() && copied->versionId != first->versionId);
        }
        const Result<ObjectData> now = await<ObjectData>([&](auto cb) { m_session->getObject(bucket, k, {}, &m_ctx, std::move(cb)); });
        VERIFY_OK(now);
        QCOMPARE(now->data, QByteArray("first version"));
        const Result<QList<VersionEntry>> all = listVersions(bucket, k);
        VERIFY_OK(all);
        QCOMPARE(all->size(), 3);
        deleteAllUnder(key(QStringLiteral("versions-restore/")), bucket);
    }

    void deleteAVersionPermanently()
    {
        const QString bucket = versionedBucket();
        if (bucket.isEmpty())
            QSKIP("no versioned bucket on this backend");
        const QString k = key(QStringLiteral("versions-delete/doc.txt"));
        const Result<PutObjectResult> v1 = putIn(bucket, k, "one");
        VERIFY_OK(v1);
        const Result<PutObjectResult> v2 = putIn(bucket, k, "two");
        VERIFY_OK(v2);

        // The current version gone: the previous one is current again.
        const Result<DeleteObjectResult> del = await<DeleteObjectResult>(
            [&](auto cb) { m_session->deleteObject(bucket, k, v2->versionId, &m_ctx, std::move(cb)); });
        VERIFY_OK(del);
        QVERIFY(!del->deleteMarker);
        QCOMPARE(currentVersion(bucket, k), v1->versionId);
        const Result<ObjectMeta> gone = await<ObjectMeta>(
            [&](auto cb) { m_session->headObject(bucket, k, v2->versionId, &m_ctx, std::move(cb)); });
        QCOMPARE(gone.error().category, ErrorCategory::NotFound);

        // The last one in a batch: nothing left of the key.
        const Result<DeleteObjectsResult> batch = await<DeleteObjectsResult>(
            [&](auto cb) { m_session->deleteObjects(bucket, {{k, v1->versionId}}, &m_ctx, std::move(cb)); });
        VERIFY_OK(batch);
        QCOMPARE(batch->failed.size(), 0);
        QCOMPARE(batch->deleted.size(), 1);
        QCOMPARE(batch->deleted.first().versionId, v1->versionId);
        const Result<QList<VersionEntry>> left = listVersions(bucket, k);
        VERIFY_OK(left);
        QVERIFY(left->isEmpty());
    }

    // The canary for PUT ?versioning: a proxy that drops the subresource
    // would execute CreateBucket. On a bucket that doesn't exist, a correct
    // server fails — and no bucket appears.
    void bucketVersioningWriteOnMissingBucketCreatesNothing()
    {
        const QString canary = QStringLiteral("qbucket-it-canary-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
        QVERIFY(safeBucketName(canary));
        const Result<SubresourceWriteResult> r = setVersioning(canary, VersioningStatus::Enabled);
        const Result<void> head = await<void>([&](auto cb) { m_session->headBucket(canary, &m_ctx, std::move(cb)); });
        if (head) {
            (void)await<void>([&](auto cb) { m_session->deleteBucket(canary, &m_ctx, std::move(cb)); });
            QFAIL("PUT ?versioning on a missing bucket created it — the proxy dropped ?versioning");
        }
        QVERIFY2(!r, "PUT ?versioning on a missing bucket succeeded");
        qInfo().noquote() << "canary answer:" << r.error().toString();
        QVERIFY(r.error().category != ErrorCategory::Protocol);
        QCOMPARE(head.error().category, ErrorCategory::NotFound);
    }

    // Suspends and re-enables versioning — only on a scratch bucket with a
    // guard object (QBUCKET_IT_VERSIONING_BUCKET), or the local versions
    // bucket created by this test. Always ends Enabled.
    void toggleVersioning()
    {
        QString bucket = env("QBUCKET_IT_VERSIONING_BUCKET");
        const QString guard = QStringLiteral("guard/DO-NOT-DELETE.txt");
        if (bucket.isEmpty() && !versionedBucket().isEmpty() && m_versionBucketCreated) {
            bucket = m_versionBucket;
            VERIFY_OK(putIn(bucket, guard, "guard object: a misread write must not delete this bucket"));
        }
        if (bucket.isEmpty())
            QSKIP("no scratch bucket for versioning writes (QBUCKET_IT_VERSIONING_BUCKET)");
        QVERIFY(safeBucketName(bucket));
        const auto guardHere = [&] {
            return bool(await<ObjectMeta>([&](auto cb) { m_session->headObject(bucket, guard, {}, &m_ctx, std::move(cb)); }));
        };
        QVERIFY2(guardHere(), "refusing: the scratch bucket holds no guard object");

        const std::optional<VersioningStatus> before = versioningOf(bucket);
        QVERIFY(before.has_value());
        bool suspended = false;
        auto reenable = qScopeGuard([&] {
            if (suspended)
                (void)setVersioning(bucket, VersioningStatus::Enabled);
        });
        if (*before != VersioningStatus::Enabled) {
            VERIFY_OK(setVersioning(bucket, VersioningStatus::Enabled));
            QVERIFY(waitForVersioning(bucket, VersioningStatus::Enabled) >= 0);
        }

        suspended = true;
        VERIFY_OK(setVersioning(bucket, VersioningStatus::Suspended));
        const int toSuspended = waitForVersioning(bucket, VersioningStatus::Suspended);
        qInfo() << "read back Suspended after" << toSuspended << "s";
        QVERIFY(toSuspended >= 0);
        QVERIFY(guardHere());

        VERIFY_OK(setVersioning(bucket, VersioningStatus::Enabled));
        suspended = false;
        const int toEnabled = waitForVersioning(bucket, VersioningStatus::Enabled);
        qInfo() << "read back Enabled after" << toEnabled << "s";
        QVERIFY(toEnabled >= 0);
        QVERIFY(guardHere());
        VERIFY_OK((await<void>([&](auto cb) { m_session->headBucket(bucket, &m_ctx, std::move(cb)); })));
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
