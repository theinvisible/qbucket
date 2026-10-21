// transfer::Uploader against FakeObjectStore: single PUTs, multipart,
// retries, throttling, lost answers, pause, cancel and resume.

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Hasher.h>
#include <qbucket/s3/transfer/JsonFileResumeStore.h>
#include <qbucket/s3/transfer/Uploader.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
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

// Small parts, so multipart tests stay fast.
class SmallPartsProfile final : public GenericS3Profile {
public:
    [[nodiscard]] qint64 minPartSize() const override { return 16 * kKiB; }
};

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

} // namespace

class TestUploader : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::shared_ptr<JsonFileResumeStore> m_resume;
    std::unique_ptr<AdaptiveConcurrency> m_limiter;

    std::unique_ptr<Session> makeSession(int maxAttempts = 4)
    {
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<SmallPartsProfile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.transferTimeoutMs = 10000;
        o.retryBaseDelayMs = 5;
        o.maxAttempts = maxAttempts;
        o.transferLanes = 2;
        return std::make_unique<Session>(o);
    }

    QString writeFile(const QString& name, const QByteArray& data)
    {
        QString path = m_dir->filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return {};
        return path;
    }

    UploadRequest request(const QString& path, const QString& key, const QString& id = QStringLiteral("t1"))
    {
        UploadRequest r;
        r.bucket = kBucket;
        r.key = key;
        r.localPath = path;
        r.contentType = QStringLiteral("application/x-test");
        r.planner.multipartThreshold = 64 * kKiB;
        r.planner.minPartSize = 32 * kKiB;
        r.transferId = id;
        r.account = QStringLiteral("acct");
        return r;
    }

    std::unique_ptr<Uploader> uploader(const UploadRequest& r)
    {
        return std::make_unique<Uploader>(m_session.get(), r, m_limiter.get(), m_resume);
    }

    static bool runToEnd(Uploader& up, int timeoutMs = 20000)
    {
        QSignalSpy finished(&up, &Uploader::finished);
        up.start();
        return finished.wait(timeoutMs) || finished.size() == 1;
    }

    // Parts per part number the store executed.
    QMap<int, int> partHits() const
    {
        QMap<int, int> out;
        for (const auto& [id, n] : m_store->partUploads())
            ++out[n];
        return out;
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
        m_store->setMinPartSize(16 * kKiB);
        m_session = makeSession();
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_resume = std::make_shared<JsonFileResumeStore>(m_dir->filePath(QStringLiteral("resume")));
        m_limiter = std::make_unique<AdaptiveConcurrency>();
    }

    void cleanup()
    {
        QVERIFY(flushResumeWrites(5000));
        m_session.reset();
        m_store.reset();
        delete m_server;
        m_server = nullptr;
        m_limiter.reset();
        m_resume.reset();
        m_dir.reset();
    }

    void smallFileIsOnePut()
    {
        const QByteArray data = pattern(10 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("a.bin"), data), QStringLiteral("dir/a.bin")));
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        QCOMPARE(up->result()->integrity, Integrity::Md5Verified);
        QVERIFY(!up->result()->multipart);
        QCOMPARE(m_store->count(QStringLiteral("PutObject")), 1);
        QCOMPARE(m_store->count(QStringLiteral("CreateMultipartUpload")), 0);
        const auto o = m_store->object(kBucket, QStringLiteral("dir/a.bin"));
        QVERIFY(o);
        QCOMPARE(o->data, data);
        QCOMPARE(o->contentType, QStringLiteral("application/x-test"));
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());   // nothing to resume for one PUT
        QCOMPARE(m_limiter->inFlight(), 0);
    }

    void emptyFile()
    {
        auto up = uploader(request(writeFile(QStringLiteral("empty"), QByteArray()), QStringLiteral("empty")));
        QVERIFY(runToEnd(*up));
        QVERIFY(up->result());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("empty"))->size, 0);
    }

    void multipartUpload()
    {
        const QByteArray data = pattern(200 * kKiB + 7);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QList<qint64> progress;
        connect(up.get(), &Uploader::progress, this, [&](qint64 done, qint64 total) {
            QCOMPARE(total, data.size());
            progress.append(done);
        });
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        QVERIFY(up->result()->multipart);
        QCOMPARE(up->result()->parts, 7);
        QCOMPARE(up->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->partSizes.size(), 7);
        QCOMPARE(m_store->count(QStringLiteral("UploadPart")), 7);
        QVERIFY(m_store->uploadIds(kBucket).isEmpty());
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last(), data.size());
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());   // done: the record is gone
        QCOMPARE(m_limiter->inFlight(), 0);
        for (const RecordedRequest& r : m_server->requests()) {
            if (r.method == "PUT")
                QVERIFY(!r.header("content-md5").isEmpty());
        }
    }

    void failedPartIsRetriedLater()
    {
        m_session = makeSession(1);   // no retries inside the Session
        m_store->addFault({QStringLiteral("UploadPart"), 3, 0, 1, false, FakeResponse::s3Error(500, "InternalError")});
        const QByteArray data = pattern(150 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QVERIFY(runToEnd(*up));
        QVERIFY(up->result());
        QCOMPARE(partHits().value(3), 1);   // the faulted attempt never reached the store
        QCOMPARE(m_store->count(QStringLiteral("UploadPart")), 6);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
    }

    void throttlingHalvesTheParallelism()
    {
        m_store->addFault({QStringLiteral("UploadPart"), 0, 0, 4, false, FakeResponse::s3Error(503, "SlowDown")});
        const QByteArray data = pattern(200 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QVERIFY(runToEnd(*up));
        QVERIFY(up->result());
        QVERIFY(m_limiter->limit() < 4);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
    }

    void changingSourceIsCaughtByContentMd5()
    {
        m_store->addFault({QStringLiteral("UploadPart"), 2, 0, 1, false, FakeResponse::s3Error(400, "BadDigest")});
        const QByteArray data = pattern(150 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QVERIFY(runToEnd(*up));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::LocalIo);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());   // aborted on the server
        QVERIFY(!m_store->object(kBucket, QStringLiteral("big.bin")));
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
    }

    void sourceTouchedDuringUploadFails()
    {
        const QByteArray data = pattern(150 * kKiB);
        const QString path = writeFile(QStringLiteral("big.bin"), data);
        auto up = uploader(request(path, QStringLiteral("big.bin")));
        connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
            if (up->partsDone() == 2) {
                QFile f(path);
                if (f.open(QIODevice::ReadWrite))
                    f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(60), QFileDevice::FileModificationTime);
            }
        });
        QVERIFY(runToEnd(*up));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::LocalIo);
    }

    void completeWith200ErrorIsResolvedAndRetried()
    {
        m_store->addFault({QStringLiteral("CompleteMultipartUpload"), 0, 0, 1, false,
                           FakeResponse::ok("<Error><Code>InternalError</Code><Message>try again</Message></Error>")});
        const QByteArray data = pattern(100 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        QCOMPARE(m_store->count(QStringLiteral("CompleteMultipartUpload")), 2);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
    }

    void lostCompleteAnswerIsResolvedByHead()
    {
        FakeResponse lost;
        lost.dropWithoutResponse = true;
        m_store->addFault({QStringLiteral("CompleteMultipartUpload"), 0, 0, 1, true, lost});
        const QByteArray data = pattern(100 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        QCOMPARE(up->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(m_store->count(QStringLiteral("CompleteMultipartUpload")), 1);   // never blindly again
        QVERIFY(m_store->count(QStringLiteral("HeadObject")) >= 1);
    }

    void ifNoneMatchRefusesAnExistingKey()
    {
        m_store->putObject(kBucket, QStringLiteral("taken"), "old");
        UploadRequest r = request(writeFile(QStringLiteral("a"), "new"), QStringLiteral("taken"));
        r.ifNoneMatch = true;
        auto up = uploader(r);
        QVERIFY(runToEnd(*up));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("taken"))->data, QByteArray("old"));

        // Multipart: refused at Complete, the upload is aborted.
        const QByteArray data = pattern(100 * kKiB);
        UploadRequest big = request(writeFile(QStringLiteral("big"), data), QStringLiteral("taken"), QStringLiteral("t2"));
        big.ifNoneMatch = true;
        auto up2 = uploader(big);
        QVERIFY(runToEnd(*up2));
        QVERIFY(!up2->result());
        QCOMPARE(up2->result().error().category, ErrorCategory::PreconditionFailed);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("taken"))->data, QByteArray("old"));
    }

    void replaceKeepsEveryHeaderAndChecksTheETag()
    {
        m_store->putObject(kBucket, QStringLiteral("doc.html"), "old");
        const QString etag = m_store->object(kBucket, QStringLiteral("doc.html"))->etag;
        ObjectHeaders h;
        h.contentType = QStringLiteral("text/html");
        h.cacheControl = QStringLiteral("max-age=60");
        h.userMetadata.insert(QStringLiteral("origin"), QStringLiteral("editor"));
        const auto replace = [&](const QByteArray& data, const QString& ifMatch, const QString& id) {
            UploadRequest r = request(writeFile(id, data), QStringLiteral("doc.html"), id);
            r.contentType.clear();
            r.headers = h;
            r.cannedAcl = QStringLiteral("public-read");
            r.serverSideEncryption = QStringLiteral("AES256");
            r.ifMatch = ifMatch;
            return uploader(r);
        };

        // Single PUT.
        auto up = replace("new content", etag, QStringLiteral("r1"));
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        auto o = m_store->object(kBucket, QStringLiteral("doc.html"));
        QCOMPARE(o->data, QByteArray("new content"));
        QCOMPARE(o->contentType, QStringLiteral("text/html"));
        QCOMPARE(o->headers.value(QStringLiteral("cache-control")), QStringLiteral("max-age=60"));
        QCOMPARE(o->metadata.value(QStringLiteral("origin")), QStringLiteral("editor"));
        QCOMPARE(o->acl, QStringLiteral("public-read"));
        QCOMPARE(o->headers.value(QStringLiteral("x-amz-server-side-encryption")), QStringLiteral("AES256"));

        // The old ETag is stale now: refused, nothing changes.
        up = replace("other", etag, QStringLiteral("r2"));
        QVERIFY(runToEnd(*up));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc.html"))->data, QByteArray("new content"));

        // Multipart: the same headers on CreateMultipartUpload, the condition on Complete.
        const QByteArray big = pattern(100 * kKiB);
        up = replace(big, etag, QStringLiteral("r3"));
        QVERIFY(runToEnd(*up));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::PreconditionFailed);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());
        const QString current = m_store->object(kBucket, QStringLiteral("doc.html"))->etag;
        up = replace(big, current, QStringLiteral("r4"));
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        QVERIFY(up->result()->multipart);
        o = m_store->object(kBucket, QStringLiteral("doc.html"));
        QCOMPARE(o->data, big);
        QCOMPARE(o->contentType, QStringLiteral("text/html"));
        QCOMPARE(o->headers.value(QStringLiteral("cache-control")), QStringLiteral("max-age=60"));
        QCOMPARE(o->metadata.value(QStringLiteral("origin")), QStringLiteral("editor"));
        QCOMPARE(o->acl, QStringLiteral("public-read"));
    }

    void conditionalPutRefusedOnItsOwnRetryIsResolvedByHead()
    {
        // The PUT lands, its answer is lost, the Session retries — and the
        // server refuses the retry because the object is ours now.
        m_store->putObject(kBucket, QStringLiteral("doc"), "old");
        FakeResponse lost;
        lost.dropWithoutResponse = true;
        m_store->addFault({QStringLiteral("PutObject"), 0, 0, 1, true, lost});
        UploadRequest r = request(writeFile(QStringLiteral("a"), "mine"), QStringLiteral("doc"));
        r.ifMatch = m_store->object(kBucket, QStringLiteral("doc"))->etag;
        auto up = uploader(r);
        QVERIFY(runToEnd(*up));
        if (!up->result())
            QFAIL(qPrintable(up->result().error().toString()));
        QCOMPARE(up->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(m_store->count(QStringLiteral("PutObject")), 2);
        QVERIFY(m_store->count(QStringLiteral("HeadObject")) >= 1);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->data, QByteArray("mine"));

        // Somebody else's object behind the refusal stays a refusal.
        m_store->putObject(kBucket, QStringLiteral("doc2"), "old");
        UploadRequest r2 = request(writeFile(QStringLiteral("b"), "mine"), QStringLiteral("doc2"), QStringLiteral("t2"));
        r2.ifMatch = QStringLiteral("\"0123456789abcdef0123456789abcdef\"");
        auto up2 = uploader(r2);
        QVERIFY(runToEnd(*up2));
        QVERIFY(!up2->result());
        QCOMPARE(up2->result().error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc2"))->data, QByteArray("old"));
    }

    void missingFileFails()
    {
        auto up = uploader(request(m_dir->filePath(QStringLiteral("nope")), QStringLiteral("k")));
        QVERIFY(runToEnd(*up));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::LocalIo);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void pauseAndResume()
    {
        const QByteArray data = pattern(200 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QSignalSpy paused(up.get(), &Uploader::paused);
        QSignalSpy finished(up.get(), &Uploader::finished);
        bool pausedOnce = false;
        connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
            if (!pausedOnce && up->partsDone() >= 1) {
                pausedOnce = true;
                up->pause();
            }
        });
        up->start();
        QVERIFY(paused.wait(10000));
        QCOMPARE(up->state(), Uploader::State::Paused);
        const int sent = m_store->count(QStringLiteral("UploadPart"));
        QTest::qWait(300);
        QCOMPARE(m_store->count(QStringLiteral("UploadPart")), sent);   // nothing new while paused
        QVERIFY(finished.isEmpty());
        QVERIFY(sent < 7);
        up->resume();
        QVERIFY(finished.wait(20000));
        QVERIFY(up->result());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
    }

    void cancelAbortsTheUpload()
    {
        const QByteArray data = pattern(200 * kKiB);
        auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
        QSignalSpy finished(up.get(), &Uploader::finished);
        connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
            if (up->partsDone() >= 2)
                up->cancel();
        });
        up->start();
        QVERIFY(finished.wait(10000));
        QVERIFY(!up->result());
        QCOMPARE(up->result().error().category, ErrorCategory::Cancelled);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());
        QVERIFY(!m_store->object(kBucket, QStringLiteral("big.bin")));
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
        QTRY_COMPARE(m_limiter->inFlight(), 0);
    }

    void resumeAfterAStop()
    {
        const QByteArray data = pattern(300 * kKiB);
        const QString path = writeFile(QStringLiteral("big.bin"), data);
        {
            auto up = uploader(request(path, QStringLiteral("big.bin")));
            QSignalSpy finished(up.get(), &Uploader::finished);
            connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
                if (up->partsDone() >= 4)
                    up->cancel(true);   // the application quits
            });
            up->start();
            QVERIFY(finished.wait(10000));
            QCOMPARE(up->result().error().category, ErrorCategory::Cancelled);
        }
        QVERIFY(flushResumeWrites(5000));
        QCOMPARE(m_resume->ids(), QStringList{QStringLiteral("t1")});
        QCOMPARE(m_store->uploadIds(kBucket).size(), 1);
        const QMap<int, int> before = partHits();
        const auto recorded = m_resume->load(QStringLiteral("t1"));
        QVERIFY(recorded);
        QVERIFY(recorded->parts.size() >= 4);

        // A restart: only the record knows what to do.
        auto again = Uploader::fromStore(m_session.get(), m_resume, QStringLiteral("t1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        if (!again->result())
            QFAIL(qPrintable(again->result().error().toString()));
        QVERIFY(again->result()->resumed);
        QVERIFY(!again->result()->restarted);
        QCOMPARE(again->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->contentType, QStringLiteral("application/x-test"));
        // Recorded parts were not sent again.
        const QMap<int, int> after = partHits();
        for (const PartRecord& p : recorded->parts)
            QCOMPARE(after.value(p.partNumber), before.value(p.partNumber));
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
    }

    // While ListParts is still out, nothing may start: another transfer
    // freeing a slot of the shared limiter used to pump the parts early, the
    // server's parts were then counted twice, and Complete went out with a
    // part missing (seen under load in test_upload_job).
    void resumeWaitsForTheServersParts()
    {
        const QByteArray data = pattern(300 * kKiB);
        const QString path = writeFile(QStringLiteral("big.bin"), data);
        {
            auto up = uploader(request(path, QStringLiteral("big.bin")));
            QSignalSpy finished(up.get(), &Uploader::finished);
            connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
                if (up->partsDone() >= 4)
                    up->cancel(true);
            });
            up->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        const QMap<int, int> before = partHits();
        const auto recorded = m_resume->load(QStringLiteral("t1"));
        QVERIFY(recorded);

        m_store->setDelay(QStringLiteral("ListParts"), 400);
        auto again = Uploader::fromStore(m_session.get(), m_resume, QStringLiteral("t1"), m_limiter.get());
        // Another transfer of the account finishes a part meanwhile.
        QTimer::singleShot(150, this, [this] {
            if (const auto slot = m_limiter->tryAcquire())
                m_limiter->release(*slot, AdaptiveConcurrency::Outcome::Succeeded);
        });
        QVERIFY(runToEnd(*again));
        if (!again->result())
            QFAIL(qPrintable(again->result().error().toString()));
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
        const QMap<int, int> after = partHits();
        for (const PartRecord& p : recorded->parts)
            QCOMPARE(after.value(p.partNumber), before.value(p.partNumber));
    }

    void resumeAdoptsPartsTheRecordMissed()
    {
        const QByteArray data = pattern(300 * kKiB);
        const QString path = writeFile(QStringLiteral("big.bin"), data);
        {
            auto up = uploader(request(path, QStringLiteral("big.bin")));
            QSignalSpy finished(up.get(), &Uploader::finished);
            connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
                if (up->partsDone() >= 5)
                    up->cancel(true);
            });
            up->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        // The process died before it could record the parts.
        QFile parts(QDir(m_resume->directory()).filePath(QStringLiteral("t1.parts")));
        QVERIFY(parts.open(QIODevice::WriteOnly | QIODevice::Truncate));
        parts.close();
        const QMap<int, int> before = partHits();
        auto again = Uploader::fromStore(m_session.get(), m_resume, QStringLiteral("t1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        QVERIFY(again->result());
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
        // What the server had and the local bytes match was kept.
        const QMap<int, int> after = partHits();
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            QCOMPARE(after.value(it.key()), it.value());
    }

    void resumeStartsOverWhenTheUploadIsGone()
    {
        const QByteArray data = pattern(200 * kKiB);
        const QString path = writeFile(QStringLiteral("big.bin"), data);
        {
            auto up = uploader(request(path, QStringLiteral("big.bin")));
            QSignalSpy finished(up.get(), &Uploader::finished);
            connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
                if (up->partsDone() >= 2)
                    up->cancel(true);
            });
            up->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        // Someone aborted it meanwhile (lifecycle rule, the incomplete-uploads view).
        const QString id = m_store->uploadIds(kBucket).value(0);
        QObject ctx;
        bool aborted = false;
        m_session->abortMultipartUpload(kBucket, QStringLiteral("big.bin"), id, &ctx, [&](const Result<void>& r) { aborted = bool(r); });
        QTRY_VERIFY(aborted);
        auto again = Uploader::fromStore(m_session.get(), m_resume, QStringLiteral("t1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        QVERIFY(again->result());
        QVERIFY(again->result()->restarted);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("big.bin"))->data, data);
    }

    void resumeRefusesAChangedSource()
    {
        const QByteArray data = pattern(200 * kKiB);
        const QString path = writeFile(QStringLiteral("big.bin"), data);
        {
            auto up = uploader(request(path, QStringLiteral("big.bin")));
            QSignalSpy finished(up.get(), &Uploader::finished);
            connect(up.get(), &Uploader::progress, this, [&](qint64, qint64) {
                if (up->partsDone() >= 2)
                    up->cancel(true);
            });
            up->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        writeFile(QStringLiteral("big.bin"), pattern(200 * kKiB, 7));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(120), QFileDevice::FileModificationTime);
        }
        auto again = Uploader::fromStore(m_session.get(), m_resume, QStringLiteral("t1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        QVERIFY(!again->result());
        QCOMPARE(again->result().error().category, ErrorCategory::LocalIo);
        QTRY_VERIFY(m_store->uploadIds(kBucket).isEmpty());   // the stale parts are aborted
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
        QVERIFY(!m_store->object(kBucket, QStringLiteral("big.bin")));
    }

    void destroyingMidwayGivesTheSlotsBack()
    {
        FakeResponse slow = FakeResponse::ok();
        slow.delayMs = 2000;
        m_store->addFault({QStringLiteral("UploadPart"), 0, 0, -1, false, slow});
        const QByteArray data = pattern(200 * kKiB);
        {
            auto up = uploader(request(writeFile(QStringLiteral("big.bin"), data), QStringLiteral("big.bin")));
            up->start();
            QTRY_VERIFY(m_limiter->inFlight() > 0);
        }
        QCOMPARE(m_limiter->inFlight(), 0);
    }

    void sharedLimiterSpansUploads()
    {
        AdaptiveConcurrency shared({2, 1, 2, 100});
        const QByteArray data = pattern(200 * kKiB);
        auto a = std::make_unique<Uploader>(m_session.get(), request(writeFile(QStringLiteral("a"), data), QStringLiteral("a"), QStringLiteral("ta")),
                                            &shared, m_resume);
        auto b = std::make_unique<Uploader>(m_session.get(), request(writeFile(QStringLiteral("b"), data), QStringLiteral("b"), QStringLiteral("tb")),
                                            &shared, m_resume);
        int maxInFlight = 0;
        connect(&shared, &AdaptiveConcurrency::slotAvailable, this, [&] { maxInFlight = std::max(maxInFlight, shared.inFlight()); });
        QTimer probe;
        connect(&probe, &QTimer::timeout, this, [&] { maxInFlight = std::max(maxInFlight, shared.inFlight()); });
        probe.start(1);
        QSignalSpy fa(a.get(), &Uploader::finished);
        QSignalSpy fb(b.get(), &Uploader::finished);
        a->start();
        b->start();
        QTRY_VERIFY_WITH_TIMEOUT(fa.size() == 1 && fb.size() == 1, 20000);
        QVERIFY(a->result());
        QVERIFY(b->result());
        QVERIFY(maxInFlight <= 2);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("a"))->data, data);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("b"))->data, data);
    }
};

QTEST_GUILESS_MAIN(TestUploader)
#include "test_uploader.moc"
