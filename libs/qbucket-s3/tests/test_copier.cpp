// transfer::Copier (UploadPartCopy) against FakeObjectStore.

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Copier.h>

#include <QSignalSpy>
#include <QtTest>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;
using namespace qbucket::s3::transfer;

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

class SmallPartsProfile final : public GenericS3Profile {
public:
    [[nodiscard]] qint64 minPartSize() const override { return 16 * kKiB; }
};

QByteArray pattern(qint64 size)
{
    QByteArray out(size, Qt::Uninitialized);
    quint32 x = 2463534242u;
    for (qint64 i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = char(x & 0xFF);
    }
    return out;
}

} // namespace

class TestCopier : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<AdaptiveConcurrency> m_limiter;
    QByteArray m_data;

    CopyRequest request(const QString& from, const QString& to)
    {
        CopyRequest r;
        r.sourceBucket = kBucket;
        r.sourceKey = from;
        r.bucket = kBucket;
        r.key = to;
        r.partSize = 64 * kKiB;
        return r;
    }

    static bool runToEnd(Copier& c)
    {
        QSignalSpy finished(&c, &Copier::finished);
        c.start();
        return finished.wait(20000) || finished.size() == 1;
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
        m_store->createBucket(QStringLiteral("other"));
        m_store->setMinPartSize(16 * kKiB);
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<SmallPartsProfile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.transferTimeoutMs = 10000;
        o.retryBaseDelayMs = 5;
        o.transferLanes = 2;
        m_session = std::make_unique<Session>(o);
        m_limiter = std::make_unique<AdaptiveConcurrency>();
        m_data = pattern(300 * kKiB + 11);
        m_store->putObject(kBucket, QStringLiteral("src"), m_data, QStringLiteral("video/mp4"));
    }

    void cleanup()
    {
        m_session.reset();
        m_store.reset();
        delete m_server;
        m_server = nullptr;
        m_limiter.reset();
    }

    void copiesInParts()
    {
        Copier c(m_session.get(), request(QStringLiteral("src"), QStringLiteral("dst/copy")), m_limiter.get());
        QList<qint64> progress;
        connect(&c, &Copier::progress, this, [&](qint64 done, qint64) { progress.append(done); });
        QVERIFY(runToEnd(c));
        if (!c.result())
            QFAIL(qPrintable(c.result().error().toString()));
        QCOMPARE(c.result()->parts, 5);
        QCOMPARE(c.result()->size, m_data.size());
        QCOMPARE(c.result()->sourceEtag, m_store->object(kBucket, QStringLiteral("src"))->etag);
        const auto copy = m_store->object(kBucket, QStringLiteral("dst/copy"));
        QVERIFY(copy);
        QCOMPARE(copy->data, m_data);
        QCOMPARE(copy->contentType, QStringLiteral("video/mp4"));   // carried over like directive COPY
        QCOMPARE(m_store->count(QStringLiteral("UploadPartCopy")), 5);
        QVERIFY(m_store->uploadIds(kBucket).isEmpty());
        QCOMPARE(progress.last(), m_data.size());
        QCOMPARE(m_limiter->inFlight(), 0);
        // Every part guarded by the source's ETag.
        int guarded = 0;
        for (const RecordedRequest& r : m_server->requests())
            guarded += r.header("x-amz-copy-source-if-match").isNull() ? 0 : 1;
        QCOMPARE(guarded, 5);
    }

    void storageClassIsKeptOrSet()
    {
        CopyRequest r = request(QStringLiteral("src"), QStringLiteral("dst/cold"));
        r.storageClass = QStringLiteral("STANDARD_IA");
        Copier c(m_session.get(), r, m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(c.result());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("dst/cold"))->storageClass, QStringLiteral("STANDARD_IA"));
    }

    void acrossBuckets()
    {
        CopyRequest r = request(QStringLiteral("src"), QStringLiteral("copy"));
        r.bucket = QStringLiteral("other");
        Copier c(m_session.get(), r, m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(c.result());
        QCOMPARE(m_store->object(QStringLiteral("other"), QStringLiteral("copy"))->data, m_data);
    }

    void staleSourceIsRefusedUpFront()
    {
        CopyRequest r = request(QStringLiteral("src"), QStringLiteral("dst"));
        r.ifMatch = QStringLiteral("\"00000000000000000000000000000000\"");
        Copier c(m_session.get(), r, m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(!c.result());
        QCOMPARE(c.result().error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->count(QStringLiteral("CreateMultipartUpload")), 0);
    }

    void sourceChangingMidwayAbortsTheCopy()
    {
        m_store->addFault({QStringLiteral("UploadPartCopy"), 0, 2, 1, false, FakeResponse::s3Error(412, "PreconditionFailed")});
        Copier c(m_session.get(), request(QStringLiteral("src"), QStringLiteral("dst")), m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(!c.result());
        QCOMPARE(c.result().error().category, ErrorCategory::PreconditionFailed);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());
        QVERIFY(!m_store->object(kBucket, QStringLiteral("dst")));
    }

    void transient200ErrorsAreRetried()
    {
        m_store->addFault({QStringLiteral("UploadPartCopy"), 3, 0, 1, false,
                           FakeResponse::ok("<Error><Code>InternalError</Code><Message>later</Message></Error>")});
        Copier c(m_session.get(), request(QStringLiteral("src"), QStringLiteral("dst")), m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(c.result());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("dst"))->data, m_data);
    }

    void permanentPartErrorsAbort()
    {
        m_store->addFault({QStringLiteral("UploadPartCopy"), 2, 0, -1, false,
                           FakeResponse::ok("<Error><Code>AccessDenied</Code><Message>no</Message></Error>")});
        Copier c(m_session.get(), request(QStringLiteral("src"), QStringLiteral("dst")), m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(!c.result());
        QCOMPARE(c.result().error().category, ErrorCategory::NotPermitted);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());
    }

    void lostCompleteAnswerIsResolved()
    {
        FakeResponse lost;
        lost.dropWithoutResponse = true;
        m_store->addFault({QStringLiteral("CompleteMultipartUpload"), 0, 0, 1, true, lost});
        Copier c(m_session.get(), request(QStringLiteral("src"), QStringLiteral("dst")), m_limiter.get());
        QVERIFY(runToEnd(c));
        if (!c.result())
            QFAIL(qPrintable(c.result().error().toString()));
        QCOMPARE(m_store->count(QStringLiteral("CompleteMultipartUpload")), 1);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("dst"))->data, m_data);
    }

    void cancelAborts()
    {
        m_store->setDelay(QStringLiteral("UploadPartCopy"), 100);
        Copier c(m_session.get(), request(QStringLiteral("src"), QStringLiteral("dst")), m_limiter.get());
        QSignalSpy finished(&c, &Copier::finished);
        c.start();
        QTRY_VERIFY(m_store->count(QStringLiteral("UploadPartCopy")) >= 1);
        c.cancel();
        QVERIFY(finished.wait(5000));
        QCOMPARE(c.result().error().category, ErrorCategory::Cancelled);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());
        QVERIFY(!m_store->object(kBucket, QStringLiteral("dst")));
        QTRY_COMPARE(m_limiter->inFlight(), 0);
    }

    void emptyObject()
    {
        m_store->putObject(kBucket, QStringLiteral("empty"), QByteArray());
        Copier c(m_session.get(), request(QStringLiteral("empty"), QStringLiteral("empty-copy")), m_limiter.get());
        QVERIFY(runToEnd(c));
        QVERIFY(c.result());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("empty-copy"))->size, 0);
    }
};

QTEST_GUILESS_MAIN(TestCopier)
#include "test_copier.moc"
