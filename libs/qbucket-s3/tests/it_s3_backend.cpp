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
#include <qbucket/s3/providers/GenericS3Profile.h>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSignalSpy>
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

#define VERIFY_OK(result) \
    QVERIFY2(result, qPrintable((result) ? QString() : (result).error().toString()))

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
        m_options.profile = std::make_shared<GenericS3Profile>();
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
        if (m_session)
            deleteAllUnder(m_prefix);
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
        o.initialClockOffsetSecs = -20 * 60;   // pretend our clock is 20 min ahead
        Session skewed(o);
        QSignalSpy skew(&skewed, &Session::clockSkewDetected);
        const Result<BucketList> r = await<BucketList>([&](auto cb) { skewed.listBuckets(&m_ctx, std::move(cb)); });
        VERIFY_OK(r);
        QVERIFY(skew.size() >= 1);
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
