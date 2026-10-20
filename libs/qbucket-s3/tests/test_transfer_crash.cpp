// A transfer survives the death of its process (PLAN.md §12.6): s3cp is
// killed mid-upload — no cleanup, no flush — and a second s3cp continues
// from the resume store. FakeObjectStore runs in this process.

#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>
#include <qbucket/s3/transfer/JsonFileResumeStore.h>

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

constexpr qint64 kMiB = Q_INT64_C(1024) * 1024;
const QString kBucket = QStringLiteral("bkt");

QByteArray pattern(qint64 size)
{
    QByteArray out(size, Qt::Uninitialized);
    quint32 x = 2463534242u;
    for (qint64 i = 0; i < size; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        for (qint64 j = i; j < std::min(size, i + 4); ++j)
            out[j] = char((x >> (8 * (j - i))) & 0xFF);
    }
    return out;
}

} // namespace

class TestTransferCrash : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    QTemporaryDir m_dir;

    QStringList commonArgs() const
    {
        return {QStringLiteral("--endpoint"), m_server->endpointUrl().toString(),
                QStringLiteral("--region"), QStringLiteral("test-1"),
                QStringLiteral("--path-style"),
                QStringLiteral("--resume-dir"), m_dir.filePath(QStringLiteral("resume")),
                QStringLiteral("--concurrency"), QStringLiteral("2"),
                QStringLiteral("--lanes"), QStringLiteral("2")};
    }

    std::unique_ptr<QProcess> s3cp(const QStringList& args)
    {
        auto p = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("AWS_ACCESS_KEY_ID"), QStringLiteral("AKIDTEST"));
        env.insert(QStringLiteral("AWS_SECRET_ACCESS_KEY"), QStringLiteral("test/secret+key"));
        env.remove(QStringLiteral("AWS_SESSION_TOKEN"));
        p->setProcessEnvironment(env);
        p->setProgram(QStringLiteral(S3CP_PATH));
        p->setArguments(commonArgs() + args);
        p->start();
        return p;
    }

    // Waits for the process while this thread keeps serving requests.
    static bool waitForExit(QProcess& p, int timeoutMs)
    {
        if (p.state() == QProcess::NotRunning)
            return true;
        QSignalSpy finished(&p, &QProcess::finished);
        return finished.wait(timeoutMs);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY2(QFile::exists(QStringLiteral(S3CP_PATH)), S3CP_PATH);
    }

    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        Credentials c;
        c.accessKeyId = QStringLiteral("AKIDTEST");
        c.secretAccessKey = QStringLiteral("test/secret+key");
        m_server->setVerifySignatures(c, "test-1");
        m_server->setRecordBodies(false);
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
    }

    void cleanup()
    {
        m_store.reset();
        delete m_server;
        m_server = nullptr;
    }

    void killedUploadResumes()
    {
        // 70 MiB → nine 8 MiB parts (the planner's default above 64 MiB).
        const QByteArray data = pattern(70 * kMiB + 123);
        const QString path = m_dir.filePath(QStringLiteral("big.bin"));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(data), data.size());
        }
        m_store->setDelay(QStringLiteral("UploadPart"), 150);   // time to kill it in between

        auto first = s3cp({QStringLiteral("--id"), QStringLiteral("crash1"), QStringLiteral("upload"), path, kBucket,
                           QStringLiteral("big.bin")});
        QVERIFY(first->waitForStarted(10000));
        QTRY_VERIFY_WITH_TIMEOUT(m_store->partUploads().size() >= 3, 30000);
        first->kill();   // SIGKILL / TerminateProcess: nothing gets cleaned up
        QVERIFY(waitForExit(*first, 10000));
        QCOMPARE(first->exitStatus(), QProcess::CrashExit);
        QVERIFY(!m_store->object(kBucket, QStringLiteral("big.bin")));
        QCOMPARE(m_store->uploadIds(kBucket).size(), 1);
        const int partsBefore = int(m_store->partUploads().size());
        QVERIFY(partsBefore < 9);

        m_store->setDelay(QStringLiteral("UploadPart"), 0);
        auto second = s3cp({QStringLiteral("resume"), QStringLiteral("crash1")});
        QVERIFY(waitForExit(*second, 60000));
        const QString output = QString::fromUtf8(second->readAllStandardOutput());
        QVERIFY2(second->exitCode() == 0, qPrintable(output + QString::fromUtf8(second->readAllStandardError())));
        QVERIFY2(output.contains(QStringLiteral("(resumed)")), qPrintable(output));
        QVERIFY2(output.contains(QStringLiteral("md5-verified")), qPrintable(output));

        const auto o = m_store->object(kBucket, QStringLiteral("big.bin"));
        QVERIFY(o);
        QCOMPARE(o->size, data.size());
        QCOMPARE(o->md5, QCryptographicHash::hash(data, QCryptographicHash::Md5));
        QVERIFY(m_store->uploadIds(kBucket).isEmpty());
        // Nothing that reached the server before the kill was sent again:
        // recorded parts are trusted, unrecorded ones adopted by their hash.
        QMap<int, int> hits;
        for (const auto& [id, n] : m_store->partUploads())
            ++hits[n];
        QCOMPARE(hits.size(), 9);
        for (auto it = hits.cbegin(); it != hits.cend(); ++it)
            QCOMPARE(it.value(), 1);
        transfer::JsonFileResumeStore store(m_dir.filePath(QStringLiteral("resume")));
        QVERIFY(store.ids().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestTransferCrash)
#include "test_transfer_crash.moc"
