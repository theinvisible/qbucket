// Streaming transfers through the Session: file bodies, downloads into
// files, progress, transfer lanes (PLAN.md §13.3).

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

constexpr qint64 kKiB = 1024;
constexpr qint64 kMiB = 1024 * kKiB;

Credentials testCredentials()
{
    Credentials c;
    c.accessKeyId = QStringLiteral("AKIDTEST");
    c.secretAccessKey = QStringLiteral("test/secret+key");
    return c;
}

// Which thread ran which operation.
class LaneRecorder final : public RequestObserver {
public:
    void onRequestFinished(const RequestRecord& r) override
    {
        QMutexLocker lock(&mutex);
        threads.append({r.operation, QThread::currentThread()});
    }
    QMutex mutex;
    QList<QPair<QString, QThread*>> threads;
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

QByteArray md5Base64(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toBase64();
}

QByteArray quotedMd5(const QByteArray& data)
{
    return '"' + QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex() + '"';
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestSessionTransfer : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::shared_ptr<LaneRecorder> m_recorder;
    std::unique_ptr<QTemporaryDir> m_dir;

    std::unique_ptr<Session> makeSession(int lanes = 0)
    {
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.observer = m_recorder;
        o.transferTimeoutMs = 10000;
        o.retryBaseDelayMs = 10;
        o.transferLanes = lanes;
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

    // A file body for [offset, offset + length) of `data`, written to `path`.
    static FileBody bodyFor(const QString& path, const QByteArray& data, qint64 offset, qint64 length)
    {
        FileBody b;
        b.path = path;
        b.offset = offset;
        b.length = length;
        const QByteArray range = data.mid(offset, length);
        b.md5Base64 = md5Base64(range);
        b.sha256Hex = QCryptographicHash::hash(range, QCryptographicHash::Sha256).toHex();
        return b;
    }

    template <typename T>
    static void waitFor(std::optional<Result<T>>& out, int ms = 10000)
    {
        QTRY_VERIFY_WITH_TIMEOUT(out.has_value(), ms);
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_recorder = std::make_shared<LaneRecorder>();
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
    }

    void cleanup()
    {
        delete m_server;
        m_server = nullptr;
        m_dir.reset();
    }

    // --- uploads from files --------------------------------------------------------

    void putStreamsTheFileRange()
    {
        const QByteArray data = pattern(3 * kMiB);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        m_server->on("PUT", QStringLiteral("^/bkt/obj$"), FakeResponse::ok().withHeader("ETag", "\"abc\""));
        auto session = makeSession();
        QVERIFY(session->requiresPayloadHash());   // plain HTTP

        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 1024 * kKiB, 1536 * kKiB);
        QObject context;
        std::optional<Result<PutObjectResult>> out;
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        if (!*out)
            QFAIL(qPrintable(out->error().toString()));
        QCOMPARE((*out)->etag, QStringLiteral("\"abc\""));
        QVERIFY(!(*out)->throttled);

        const QList<RecordedRequest> requests = m_server->requests();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests[0].body, data.mid(1024 * kKiB, 1536 * kKiB));
        QCOMPARE(requests[0].header("content-md5"), p.file->md5Base64);
        QCOMPARE(requests[0].header("x-amz-content-sha256"), p.file->sha256Hex);
        QCOMPARE(requests[0].header("content-length").toLongLong(), 1536 * kKiB);
    }

    void plainHttpWithoutHashIsRefused()
    {
        const QByteArray data = pattern(1000);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        auto session = makeSession();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 0, data.size());
        p.file->sha256Hex.clear();
        QObject context;
        std::optional<Result<PutObjectResult>> out;
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void streamedBodyNeedsContentMd5()
    {
        const QByteArray data = pattern(1000);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        auto session = makeSession();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 0, data.size());
        p.file->md5Base64.clear();
        QObject context;
        std::optional<Result<PutObjectResult>> out;
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::InvalidRequest);

        // Data and a file at once is a caller bug as well.
        p.file = bodyFor(path, data, 0, data.size());
        p.data = "x";
        out.reset();
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void retryRereadsTheFileAndReportsThrottling()
    {
        const QByteArray data = pattern(700 * kKiB);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        m_server->addRoute({"PUT", QRegularExpression(QStringLiteral("^/bkt/obj$")), {}, {}},
                           {FakeResponse::s3Error(503, "SlowDown"), FakeResponse::ok().withHeader("ETag", "\"e\"")});
        auto session = makeSession();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 100, data.size() - 100);
        QObject context;
        std::optional<Result<PutObjectResult>> out;
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        if (!*out)
            QFAIL(qPrintable(out->error().toString()));
        QVERIFY((*out)->throttled);
        const QList<RecordedRequest> requests = m_server->requests();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(requests[0].body, data.mid(100));
        QCOMPARE(requests[1].body, data.mid(100));
    }

    void shorterFileIsALocalError()
    {
        const QByteArray data = pattern(2000);
        const QString path = writeFile(QStringLiteral("src.bin"), data.left(1000));
        auto session = makeSession();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 0, data.size());
        QObject context;
        std::optional<Result<PutObjectResult>> out;
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::LocalIo);
        QCOMPARE(m_server->requestCount(), 0);   // never retried, never sent

        // A file that does not exist at all.
        p.file->path = m_dir->filePath(QStringLiteral("missing.bin"));
        out.reset();
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::LocalIo);
    }

    void uploadProgressReachesTheTotal()
    {
        const QByteArray data = pattern(6 * kMiB);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        FakeResponse slowly = FakeResponse::ok();
        m_server->on("PUT", QStringLiteral("^/bkt/obj$"), slowly);
        auto session = makeSession();
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 0, data.size());
        QObject context;
        QList<QPair<qint64, qint64>> steps;
        bool wrongThread = false;
        bool afterResult = false;
        std::optional<Result<PutObjectResult>> out;
        session->putObject(
            p, &context, [&](Result<PutObjectResult> r) { out.emplace(std::move(r)); },
            [&](qint64 done, qint64 total) {
                wrongThread |= QThread::currentThread() != thread();
                afterResult |= out.has_value();
                steps.append({done, total});
            });
        waitFor(out);
        QVERIFY(*out);
        QVERIFY(!steps.isEmpty());
        QVERIFY(!wrongThread);
        QVERIFY(!afterResult);
        QCOMPARE(steps.last().first, data.size());
        QCOMPARE(steps.last().second, data.size());
        for (qsizetype i = 1; i < steps.size(); ++i)
            QVERIFY(steps[i].first >= steps[i - 1].first);
    }

    // --- downloads into files ------------------------------------------------------

    void downloadWholeObjectAtOffset()
    {
        const QByteArray data = pattern(2 * kMiB + 17);
        m_server->on("GET", QStringLiteral("^/bkt/obj$"),
                     FakeResponse::ok(data, "application/octet-stream")
                         .withHeader("ETag", quotedMd5(data))
                         .withHeader("Last-Modified", "Wed, 21 Oct 2026 07:28:00 GMT"));
        const QString target = writeFile(QStringLiteral("dst.qbpart"), "HEADER");
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = target;
        p.fileOffset = 6;
        QObject context;
        std::optional<Result<DownloadResult>> out;
        QList<qint64> progress;
        session->getObjectToFile(
            p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); },
            [&](qint64 done, qint64) { progress.append(done); });
        waitFor(out);
        if (!*out)
            QFAIL(qPrintable(out->error().toString()));
        QCOMPARE((*out)->bytesWritten, data.size());
        QCOMPARE((*out)->objectSize, data.size());
        QCOMPARE((*out)->md5, QCryptographicHash::hash(data, QCryptographicHash::Md5));
        QCOMPARE((*out)->integrity, Integrity::Md5Verified);
        QVERIFY((*out)->meta.lastModified.isValid());
        QCOMPARE(readFile(target), QByteArray("HEADER") + data);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last(), data.size());
    }

    void downloadRange()
    {
        const QByteArray data = pattern(10000);
        // A range is answered with 206 Partial Content.
        FakeResponse partial = FakeResponse::ok(data.mid(1000, 2000), "application/octet-stream")
                                   .withHeader("Content-Range", "bytes 1000-2999/10000")
                                   .withHeader("ETag", "\"0123456789abcdef0123456789abcdef-3\"")
                                   .withHeader("x-amz-mp-parts-count", "3");
        partial.status = 206;
        m_server->addRoute({"GET", QRegularExpression(QStringLiteral("^/bkt/obj$")), {},
                            [](const RecordedRequest& r) { return r.header("range") == "bytes=1000-2999"; }},
                           {partial});
        const QString target = m_dir->filePath(QStringLiteral("dst.qbpart"));
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = target;
        p.fileOffset = 1000;
        p.rangeStart = 1000;
        p.rangeLength = 2000;
        p.ifMatch = QStringLiteral("\"0123456789abcdef0123456789abcdef-3\"");
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        if (!*out)
            QFAIL(qPrintable(out->error().toString()));
        QCOMPARE((*out)->bytesWritten, 2000);
        QCOMPARE((*out)->objectSize, 10000);
        QCOMPARE((*out)->partsCount, 3);
        QCOMPARE((*out)->integrity, Integrity::SizeVerified);
        QCOMPARE(m_server->requests().first().header("if-match"), p.ifMatch.toLatin1());
        const QByteArray onDisk = readFile(target);
        QCOMPARE(onDisk.size(), 3000);
        QCOMPARE(onDisk.mid(1000), data.mid(1000, 2000));
    }

    void ignoredRangeIsAnError()
    {
        const QByteArray data = pattern(10000);
        m_server->on("GET", QStringLiteral("^/bkt/obj$"), FakeResponse::ok(data, "application/octet-stream"));
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = m_dir->filePath(QStringLiteral("dst.qbpart"));
        p.rangeStart = 1000;
        p.rangeLength = 2000;
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::Protocol);
    }

    void errorDocumentNeverLandsInTheFile()
    {
        m_server->on("GET", QStringLiteral("^/bkt/obj$"), FakeResponse::s3Error(404, "NoSuchKey"));
        const QString target = writeFile(QStringLiteral("dst.qbpart"), "KEEP");
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = target;
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::NotFound);
        QCOMPARE(out->error().code, QStringLiteral("NoSuchKey"));
        QCOMPARE(readFile(target), QByteArray("KEEP"));
    }

    void changedObjectFailsTheIfMatch()
    {
        m_server->on("GET", QStringLiteral("^/bkt/obj$"), FakeResponse::s3Error(412, "PreconditionFailed"));
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = m_dir->filePath(QStringLiteral("dst.qbpart"));
        p.ifMatch = QStringLiteral("\"old\"");
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::PreconditionFailed);
    }

    void droppedDownloadIsRewritten()
    {
        const QByteArray data = pattern(500 * kKiB);
        FakeResponse broken = FakeResponse::ok(data, "application/octet-stream").withHeader("ETag", quotedMd5(data));
        broken.closeAfterBodyBytes = 100 * kKiB;
        m_server->addRoute({"GET", QRegularExpression(QStringLiteral("^/bkt/obj$")), {}, {}},
                           {broken, FakeResponse::ok(data, "application/octet-stream").withHeader("ETag", quotedMd5(data))});
        const QString target = m_dir->filePath(QStringLiteral("dst.qbpart"));
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = target;
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        if (!*out)
            QFAIL(qPrintable(out->error().toString()));
        QCOMPARE(m_server->requestCount(), 2);
        QCOMPARE((*out)->integrity, Integrity::Md5Verified);
        QCOMPARE(readFile(target), data);
    }

    void corruptDownloadIsDetected()
    {
        const QByteArray data = pattern(4096);
        m_server->on("GET", QStringLiteral("^/bkt/obj$"),
                     FakeResponse::ok(data, "application/octet-stream").withHeader("ETag", quotedMd5("other")));
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = m_dir->filePath(QStringLiteral("dst.qbpart"));
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::Protocol);
    }

    void unwritableTargetIsALocalError()
    {
        m_server->on("GET", QStringLiteral("^/bkt/obj$"), FakeResponse::ok("data", "application/octet-stream"));
        auto session = makeSession();
        GetObjectToFileParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.filePath = m_dir->filePath(QStringLiteral("no/such/dir/dst.qbpart"));
        QObject context;
        std::optional<Result<DownloadResult>> out;
        session->getObjectToFile(p, &context, [&](Result<DownloadResult> r) { out.emplace(std::move(r)); });
        waitFor(out);
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::LocalIo);
        QCOMPARE(m_server->requestCount(), 0);
    }

    // --- lanes ---------------------------------------------------------------------

    void transfersRunOnTheirOwnLanes()
    {
        const QByteArray data = pattern(64 * kKiB);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        FakeResponse slow = FakeResponse::ok();
        slow.delayMs = 300;
        m_server->on("PUT", QString(), slow);
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok("<ListAllMyBucketsResult/>"));
        auto session = makeSession(2);
        QObject context;
        int puts = 0;
        for (int i = 0; i < 3; ++i) {
            PutObjectParams p;
            p.bucket = QStringLiteral("bkt");
            p.key = QStringLiteral("obj%1").arg(i);
            p.file = bodyFor(path, data, 0, data.size());
            session->putObject(p, &context, [&](const Result<PutObjectResult>& r) { puts += r ? 1 : 100; });
        }
        std::optional<Result<BucketList>> listed;
        QElapsedTimer t;
        t.start();
        session->listBuckets(&context, [&](Result<BucketList> r) { listed.emplace(std::move(r)); });
        waitFor(listed);
        QVERIFY(*listed);
        QVERIFY2(t.elapsed() < 250, "browsing waited for the transfers");
        QTRY_COMPARE(puts, 3);

        QMutexLocker lock(&m_recorder->mutex);
        QThread* browse = nullptr;
        QSet<QThread*> transfer;
        for (const auto& [op, thread] : std::as_const(m_recorder->threads)) {
            if (op == QLatin1String("ListBuckets"))
                browse = thread;
            else
                transfer.insert(thread);
        }
        QVERIFY(browse);
        QVERIFY(!transfer.contains(browse));
        QCOMPARE(transfer.size(), 2);   // three concurrent uploads, two lanes
    }

    void withoutLanesTransfersShareTheBrowseThread()
    {
        const QByteArray data = pattern(1000);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        m_server->on("PUT", QString(), FakeResponse::ok());
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok("<ListAllMyBucketsResult/>"));
        auto session = makeSession(0);
        QObject context;
        std::optional<Result<PutObjectResult>> put;
        PutObjectParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = QStringLiteral("obj");
        p.file = bodyFor(path, data, 0, data.size());
        session->putObject(p, &context, [&](Result<PutObjectResult> r) { put.emplace(std::move(r)); });
        std::optional<Result<BucketList>> listed;
        session->listBuckets(&context, [&](Result<BucketList> r) { listed.emplace(std::move(r)); });
        waitFor(put);
        waitFor(listed);
        QMutexLocker lock(&m_recorder->mutex);
        QSet<QThread*> threads;
        for (const auto& entry : std::as_const(m_recorder->threads))
            threads.insert(entry.second);
        QCOMPARE(threads.size(), 1);
    }

    void shutdownStopsEveryLane()
    {
        const QByteArray data = pattern(1000);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        FakeResponse slow = FakeResponse::ok();
        slow.delayMs = 3000;
        m_server->on("", QString(), slow);
        auto session = makeSession(3);
        QSignalSpy finished(session.get(), &Session::shutdownFinished);
        QObject context;
        QList<ErrorCategory> results;
        for (int i = 0; i < 4; ++i) {
            PutObjectParams p;
            p.bucket = QStringLiteral("bkt");
            p.key = QStringLiteral("obj%1").arg(i);
            p.file = bodyFor(path, data, 0, data.size());
            session->putObject(p, &context, [&](const Result<PutObjectResult>& r) {
                results.append(r ? ErrorCategory::Internal : r.error().category);
            });
        }
        session->headBucket(QStringLiteral("bkt"), &context, [&](const Result<void>& r) {
            results.append(r ? ErrorCategory::Internal : r.error().category);
        });
        QTRY_VERIFY(m_server->requestCount() >= 3);
        QElapsedTimer t;
        t.start();
        session->shutdown();
        session->shutdown();   // twice is harmless
        QVERIFY(finished.wait(2500));
        QVERIFY(t.elapsed() < 2500);
        QTRY_COMPARE(results.size(), 5);
        for (const ErrorCategory c : std::as_const(results))
            QCOMPARE(c, ErrorCategory::Cancelled);
        QTest::qWait(100);
        QCOMPARE(finished.size(), 1);
    }

    void destroyingWithTransfersInFlight()
    {
        const QByteArray data = pattern(1000);
        const QString path = writeFile(QStringLiteral("src.bin"), data);
        FakeResponse slow = FakeResponse::ok();
        slow.delayMs = 3000;
        m_server->on("", QString(), slow);
        QObject context;
        int calls = 0;
        QElapsedTimer t;
        t.start();
        {
            auto session = makeSession(2);
            for (int i = 0; i < 3; ++i) {
                PutObjectParams p;
                p.bucket = QStringLiteral("bkt");
                p.key = QStringLiteral("obj%1").arg(i);
                p.file = bodyFor(path, data, 0, data.size());
                session->putObject(p, &context, [&](const Result<PutObjectResult>&) { ++calls; });
            }
            QTRY_VERIFY(m_server->requestCount() >= 2);
        }
        QVERIFY(t.elapsed() < 2500);
        QTest::qWait(200);
        QCOMPARE(calls, 0);
    }
};

QTEST_GUILESS_MAIN(TestSessionTransfer)
#include "test_session_transfer.moc"
