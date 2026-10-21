// transfer::Downloader against FakeObjectStore: the integrity matrix,
// conflicts, changes on the server, corruption, pause, cancel and resume.

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Downloader.h>
#include <qbucket/s3/transfer/JsonFileResumeStore.h>
#include <qbucket/s3/transfer/Uploader.h>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
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

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestDownloader : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::shared_ptr<JsonFileResumeStore> m_resume;
    std::unique_ptr<AdaptiveConcurrency> m_limiter;

    DownloadRequest request(const QString& key, const QString& target, const QString& id = QStringLiteral("d1"))
    {
        DownloadRequest r;
        r.bucket = kBucket;
        r.key = key;
        r.targetPath = m_dir->filePath(target);
        r.planner.multipartThreshold = 64 * kKiB;
        r.planner.minPartSize = 32 * kKiB;
        r.transferId = id;
        r.account = QStringLiteral("acct");
        return r;
    }

    std::unique_ptr<Downloader> downloader(const DownloadRequest& r, AdaptiveConcurrency* limiter = nullptr)
    {
        return std::make_unique<Downloader>(m_session.get(), r, limiter ? limiter : m_limiter.get(), m_resume);
    }

    static bool runToEnd(Downloader& down, int timeoutMs = 20000)
    {
        QSignalSpy finished(&down, &Downloader::finished);
        down.start();
        return finished.wait(timeoutMs) || finished.size() == 1;
    }

    // A real multipart object, through the Uploader (32 KiB parts).
    bool seedMultipart(const QString& key, const QByteArray& data)
    {
        const QString path = m_dir->filePath(QStringLiteral("seed-") + QString::number(qHash(key)));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return false;
        f.close();
        UploadRequest r;
        r.bucket = kBucket;
        r.key = key;
        r.localPath = path;
        r.planner.multipartThreshold = 64 * kKiB;
        r.planner.minPartSize = 32 * kKiB;
        Uploader up(m_session.get(), r, m_limiter.get(), nullptr);
        QSignalSpy finished(&up, &Uploader::finished);
        up.start();
        return finished.wait(20000) && up.result() && up.result()->multipart;
    }

    int gets() const { return m_store->count(QStringLiteral("GetObject")); }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
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

    void smallObjectIsOneVerifiedGet()
    {
        const QByteArray data = pattern(10 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("dir/a.bin"), data);
        auto down = downloader(request(QStringLiteral("dir/a.bin"), QStringLiteral("out/a.bin")));
        QVERIFY(runToEnd(*down));
        if (!down->result())
            QFAIL(qPrintable(down->result().error().toString()));
        QCOMPARE(down->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(gets(), 1);
        QCOMPARE(readFile(down->result()->path), data);
        QVERIFY(!QFile::exists(Downloader::partialPath(down->result()->path)));
        // mtime from Last-Modified (second resolution over HTTP).
        const QDateTime mtime = QFileInfo(down->result()->path).lastModified(QTimeZone::UTC);
        QVERIFY(std::abs(mtime.secsTo(m_store->object(kBucket, QStringLiteral("dir/a.bin"))->lastModified)) <= 1);
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
    }

    void emptyObject()
    {
        m_store->putObject(kBucket, QStringLiteral("empty"), QByteArray());
        auto down = downloader(request(QStringLiteral("empty"), QStringLiteral("empty")));
        QVERIFY(runToEnd(*down));
        QVERIFY(down->result());
        QCOMPARE(QFileInfo(down->result()->path).size(), 0);
        QCOMPARE(down->result()->integrity, Integrity::Md5Verified);
    }

    void largeSinglePartObjectInRanges()
    {
        const QByteArray data = pattern(200 * kKiB + 3);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
        QList<qint64> progress;
        connect(down.get(), &Downloader::progress, this, [&](qint64 done, qint64 total) {
            QCOMPARE(total, data.size());
            progress.append(done);
        });
        QVERIFY(runToEnd(*down));
        QVERIFY(down->result());
        QCOMPARE(down->result()->parts, 7);
        QCOMPARE(gets(), 7);
        // Ranges, then the whole file hashed against the single-part ETag.
        QCOMPARE(down->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(readFile(down->result()->path), data);
        QCOMPARE(progress.last(), data.size());
        int ranged = 0;
        for (const RecordedRequest& r : m_server->requests())
            ranged += r.method == "GET" && !r.header("range").isNull() && !r.header("if-match").isNull() ? 1 : 0;
        QCOMPARE(ranged, 7);
        QCOMPARE(m_limiter->inFlight(), 0);
    }

    void multipartObjectFollowsItsParts()
    {
        const QByteArray data = pattern(150 * kKiB);
        QVERIFY(seedMultipart(QStringLiteral("mp"), data));
        auto down = downloader(request(QStringLiteral("mp"), QStringLiteral("mp")));
        QVERIFY(runToEnd(*down));
        if (!down->result())
            QFAIL(qPrintable(down->result().error().toString()));
        QCOMPARE(down->result()->parts, 5);   // the object's own parts
        QCOMPARE(down->result()->integrity, Integrity::Md5Verified);   // MD5-of-MD5s
        QCOMPARE(readFile(down->result()->path), data);
    }

    void irregularPartsAreOnlySizeVerified()
    {
        // Parts of 40, 20 and 40 KiB: HEAD ?partNumber=1 suggests 40 KiB
        // parts, the MD5-of-MD5s of those ranges doesn't match — honest
        // "size verified", content still exact.
        const QByteArray data = pattern(100 * kKiB);
        const QString path = m_dir->filePath(QStringLiteral("irregular"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
        f.close();
        QObject ctx;
        std::optional<Result<CreateMultipartUploadResult>> created;
        m_session->createMultipartUpload({kBucket, QStringLiteral("irr"), {}, {}, {}, {}, {}}, &ctx,
                                         [&](Result<CreateMultipartUploadResult> r) { created.emplace(std::move(r)); });
        QTRY_VERIFY(created.has_value());
        QVERIFY(*created);
        const qint64 sizes[] = {40 * kKiB, 20 * kKiB, 40 * kKiB};
        QList<CompletedPart> parts;
        qint64 offset = 0;
        for (int n = 1; n <= 3; ++n) {
            UploadPartParams up;
            up.bucket = kBucket;
            up.key = QStringLiteral("irr");
            up.uploadId = (*created)->uploadId;
            up.partNumber = n;
            up.file.path = path;
            up.file.offset = offset;
            up.file.length = sizes[n - 1];
            const QByteArray range = data.mid(offset, sizes[n - 1]);
            up.file.md5Base64 = QCryptographicHash::hash(range, QCryptographicHash::Md5).toBase64();
            up.file.sha256Hex = QCryptographicHash::hash(range, QCryptographicHash::Sha256).toHex();
            std::optional<Result<UploadPartResult>> r;
            m_session->uploadPart(up, &ctx, [&](Result<UploadPartResult> x) { r.emplace(std::move(x)); });
            QTRY_VERIFY(r.has_value());
            QVERIFY(*r);
            parts.append({n, (*r)->etag});
            offset += sizes[n - 1];
        }
        std::optional<Result<CompleteMultipartUploadResult>> done;
        m_session->completeMultipartUpload({kBucket, QStringLiteral("irr"), (*created)->uploadId, parts, false}, &ctx,
                                           [&](Result<CompleteMultipartUploadResult> r) { done.emplace(std::move(r)); });
        QTRY_VERIFY(done.has_value());
        QVERIFY(*done);

        auto down = downloader(request(QStringLiteral("irr"), QStringLiteral("irr")));
        QVERIFY(runToEnd(*down));
        QVERIFY(down->result());
        QCOMPARE(down->result()->integrity, Integrity::SizeVerified);
        QCOMPARE(readFile(down->result()->path), data);
    }

    void existingTargetIsNeverOverwrittenUnasked()
    {
        m_store->putObject(kBucket, QStringLiteral("a"), "server");
        QFile f(m_dir->filePath(QStringLiteral("a")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("local");
        f.close();
        auto down = downloader(request(QStringLiteral("a"), QStringLiteral("a")));
        QVERIFY(runToEnd(*down));
        QVERIFY(!down->result());
        QCOMPARE(down->result().error().category, ErrorCategory::Conflict);
        QCOMPARE(m_server->requestCount(), 0);
        QCOMPARE(readFile(m_dir->filePath(QStringLiteral("a"))), QByteArray("local"));

        DownloadRequest r = request(QStringLiteral("a"), QStringLiteral("a"), QStringLiteral("d2"));
        r.overwrite = true;
        auto again = downloader(r);
        QVERIFY(runToEnd(*again));
        QVERIFY(again->result());
        QCOMPARE(readFile(m_dir->filePath(QStringLiteral("a"))), QByteArray("server"));
    }

    void expectedEtagGuardsAgainstAStaleListing()
    {
        m_store->putObject(kBucket, QStringLiteral("a"), "now");
        DownloadRequest r = request(QStringLiteral("a"), QStringLiteral("a"));
        r.expectEtag = QStringLiteral("\"0123456789abcdef0123456789abcdef\"");
        auto down = downloader(r);
        QVERIFY(runToEnd(*down));
        QVERIFY(!down->result());
        QCOMPARE(down->result().error().category, ErrorCategory::PreconditionFailed);
        QCOMPARE(gets(), 0);
    }

    void objectChangedDuringTheDownload()
    {
        const QByteArray data = pattern(200 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->addFault({QStringLiteral("GetObject"), 0, 2, 1, false, FakeResponse::s3Error(412, "PreconditionFailed")});
        auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
        QVERIFY(runToEnd(*down));
        QVERIFY(!down->result());
        QCOMPARE(down->result().error().category, ErrorCategory::PreconditionFailed);
        QVERIFY(!QFile::exists(m_dir->filePath(QStringLiteral("big"))));
        QTRY_VERIFY(!QFile::exists(Downloader::partialPath(m_dir->filePath(QStringLiteral("big")))));
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
    }

    void corruptSingleGetIsFetchedAgain()
    {
        const QByteArray data = pattern(10 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("a"), data);
        const QString etag = m_store->object(kBucket, QStringLiteral("a"))->etag;
        QByteArray bad = data;
        bad[100] = char(bad[100] ^ 0x55);
        m_store->addFault({QStringLiteral("GetObject"), 0, 0, 1, false,
                           FakeResponse::ok(bad, "application/octet-stream").withHeader("ETag", etag.toLatin1())});
        auto down = downloader(request(QStringLiteral("a"), QStringLiteral("a")));
        QVERIFY(runToEnd(*down));
        QVERIFY(down->result());
        QCOMPARE(gets(), 2);
        QCOMPARE(readFile(down->result()->path), data);
    }

    void corruptRangeFailsTheWholeFileCheck()
    {
        const QByteArray data = pattern(100 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        const QString etag = m_store->object(kBucket, QStringLiteral("big"))->etag;
        // With one request at a time, the second GET is the second range.
        QByteArray bad = data.mid(32 * kKiB, 32 * kKiB);
        bad[7] = char(bad[7] ^ 0x01);
        FakeResponse wrong = FakeResponse::ok(bad, "application/octet-stream")
                                 .withHeader("ETag", etag.toLatin1())
                                 .withHeader("Content-Range", "bytes 32768-65535/102400");
        wrong.status = 206;
        m_store->addFault({QStringLiteral("GetObject"), 0, 1, 1, false, wrong});
        AdaptiveConcurrency one({1, 1, 1, 100});
        auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")), &one);
        QVERIFY(runToEnd(*down));
        QVERIFY(!down->result());
        QCOMPARE(down->result().error().category, ErrorCategory::Protocol);
        QVERIFY(!QFile::exists(m_dir->filePath(QStringLiteral("big"))));
        QVERIFY(QFile::exists(Downloader::partialPath(m_dir->filePath(QStringLiteral("big")))));   // for inspection
    }

    void missingObjectFails()
    {
        auto down = downloader(request(QStringLiteral("nope"), QStringLiteral("nope")));
        QVERIFY(runToEnd(*down));
        QVERIFY(!down->result());
        QCOMPARE(down->result().error().category, ErrorCategory::NotFound);
        QVERIFY(!QFile::exists(Downloader::partialPath(m_dir->filePath(QStringLiteral("nope")))));
    }

    void pauseAndResume()
    {
        const QByteArray data = pattern(300 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->setDelay(QStringLiteral("GetObject"), 30);
        auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
        QSignalSpy paused(down.get(), &Downloader::paused);
        QSignalSpy finished(down.get(), &Downloader::finished);
        bool once = false;
        connect(down.get(), &Downloader::progress, this, [&](qint64, qint64) {
            if (!once && down->partsDone() >= 1) {
                once = true;
                down->pause();
            }
        });
        down->start();
        QVERIFY(paused.wait(10000));
        const int sent = gets();
        QTest::qWait(300);
        QCOMPARE(gets(), sent);
        QVERIFY(finished.isEmpty());
        down->resume();
        QVERIFY(finished.wait(20000));
        QVERIFY(down->result());
        QCOMPARE(readFile(down->result()->path), data);
    }

    void cancelRemovesThePartialFile()
    {
        const QByteArray data = pattern(300 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->setDelay(QStringLiteral("GetObject"), 30);
        auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
        QSignalSpy finished(down.get(), &Downloader::finished);
        connect(down.get(), &Downloader::progress, this, [&](qint64, qint64) {
            if (down->partsDone() >= 2)
                down->cancel();
        });
        down->start();
        QVERIFY(finished.wait(10000));
        QCOMPARE(down->result().error().category, ErrorCategory::Cancelled);
        QTRY_VERIFY(!QFile::exists(Downloader::partialPath(m_dir->filePath(QStringLiteral("big")))));
        QVERIFY(!QFile::exists(m_dir->filePath(QStringLiteral("big"))));
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
        QTRY_COMPARE(m_limiter->inFlight(), 0);
    }

    void resumeAfterAStop()
    {
        const QByteArray data = pattern(300 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->setDelay(QStringLiteral("GetObject"), 20);
        {
            auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
            QSignalSpy finished(down.get(), &Downloader::finished);
            connect(down.get(), &Downloader::progress, this, [&](qint64, qint64) {
                if (down->partsDone() >= 4)
                    down->cancel(true);
            });
            down->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        QCOMPARE(m_resume->ids(), QStringList{QStringLiteral("d1")});
        const auto recorded = m_resume->load(QStringLiteral("d1"));
        QVERIFY(recorded);
        const qsizetype have = recorded->parts.size();
        QVERIFY(have >= 4);
        m_store->setDelay(QStringLiteral("GetObject"), 0);
        // Requests the first run had on the wire when it stopped reach the
        // server (in this thread) only now.
        QTest::qWait(300);
        const int before = gets();

        auto again = Downloader::fromStore(m_session.get(), m_resume, QStringLiteral("d1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        if (!again->result())
            QFAIL(qPrintable(again->result().error().toString()));
        QVERIFY(again->result()->resumed);
        QCOMPARE(again->result()->integrity, Integrity::Md5Verified);
        QCOMPARE(readFile(again->result()->path), data);
        QCOMPARE(gets() - before, 10 - int(have));   // only what was missing
        QVERIFY(flushResumeWrites(5000));
        QVERIFY(m_resume->ids().isEmpty());
    }

    void resumeRechecksWhatIsOnDisk()
    {
        const QByteArray data = pattern(300 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->setDelay(QStringLiteral("GetObject"), 20);
        {
            auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
            QSignalSpy finished(down.get(), &Downloader::finished);
            connect(down.get(), &Downloader::progress, this, [&](qint64, qint64) {
                if (down->partsDone() >= 4)
                    down->cancel(true);
            });
            down->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        const auto recorded = m_resume->load(QStringLiteral("d1"));
        QVERIFY(recorded && !recorded->parts.isEmpty());
        // The disk lost a write the record knows about.
        {
            QFile f(Downloader::partialPath(m_dir->filePath(QStringLiteral("big"))));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(recorded->parts.first().offset + 10));
            f.write("XXXX");
        }
        m_store->setDelay(QStringLiteral("GetObject"), 0);
        auto again = Downloader::fromStore(m_session.get(), m_resume, QStringLiteral("d1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        QVERIFY(again->result());
        QCOMPARE(readFile(again->result()->path), data);
    }

    void resumeStartsOverWhenTheObjectChanged()
    {
        const QByteArray data = pattern(300 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->setDelay(QStringLiteral("GetObject"), 20);
        {
            auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
            QSignalSpy finished(down.get(), &Downloader::finished);
            connect(down.get(), &Downloader::progress, this, [&](qint64, qint64) {
                if (down->partsDone() >= 2)
                    down->cancel(true);
            });
            down->start();
            QVERIFY(finished.wait(10000));
        }
        QVERIFY(flushResumeWrites(5000));
        const QByteArray newer = pattern(250 * kKiB, 99);
        m_store->putObject(kBucket, QStringLiteral("big"), newer);
        m_store->setDelay(QStringLiteral("GetObject"), 0);
        auto again = Downloader::fromStore(m_session.get(), m_resume, QStringLiteral("d1"), m_limiter.get());
        QVERIFY(runToEnd(*again));
        if (!again->result())
            QFAIL(qPrintable(again->result().error().toString()));
        QVERIFY(again->result()->restarted);
        QCOMPARE(readFile(again->result()->path), newer);
    }

    void destroyingMidwayGivesTheSlotsBack()
    {
        const QByteArray data = pattern(300 * kKiB);
        m_store->putObject(kBucket, QStringLiteral("big"), data);
        m_store->setDelay(QStringLiteral("GetObject"), 2000);
        {
            auto down = downloader(request(QStringLiteral("big"), QStringLiteral("big")));
            down->start();
            QTRY_VERIFY(m_limiter->inFlight() > 0);
        }
        QCOMPARE(m_limiter->inFlight(), 0);
    }
};

QTEST_GUILESS_MAIN(TestDownloader)
#include "test_downloader.moc"
