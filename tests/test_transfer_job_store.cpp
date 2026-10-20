#include "controllers/TransferJobStore.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace qbucket;

namespace {

TransferJobStore::Record sample(const QString& id)
{
    TransferJobStore::Record r;
    r.id = id;
    r.kind = Job::TransferKind::Upload;
    r.account = QUuid::createUuid();
    r.bucket = QStringLiteral("bkt");
    r.title = QStringLiteral("Upload “photos” to s3://bkt/");
    r.prefix = QStringLiteral("backup/");
    r.policy = Job::ConflictPolicy::KeepBoth;
    r.items = 1234567;
    r.bytes = Q_INT64_C(53687091200);
    r.started = QDateTime(QDate(2026, 10, 20), QTime(10, 0), QTimeZone::UTC);
    return r;
}

} // namespace

class TestTransferJobStore : public QObject {
    Q_OBJECT

private slots:
    void recordsRoundTrip()
    {
        QTemporaryDir dir;
        TransferJobStore store(dir.path());
        const TransferJobStore::Record r = sample(QStringLiteral("abc12345"));
        store.save(r);
        QFile plan(store.planPath(r.id));
        QVERIFY(plan.open(QIODevice::WriteOnly));
        plan.write("x\n");
        plan.close();
        store.save(sample(QStringLiteral("noplan01")));   // no plan: can't be continued

        std::optional<QList<TransferJobStore::Record>> loaded;
        QObject ctx;
        store.load(&ctx, [&](const QList<TransferJobStore::Record>& l) { loaded = l; });
        QTRY_VERIFY(loaded.has_value());
        QCOMPARE(loaded->size(), 1);
        const TransferJobStore::Record& got = loaded->first();
        QCOMPARE(got.id, r.id);
        QCOMPARE(got.kind, Job::TransferKind::Upload);
        QCOMPARE(got.account, r.account);
        QCOMPARE(got.prefix, r.prefix);
        QCOMPARE(got.policy, Job::ConflictPolicy::KeepBoth);
        QCOMPARE(got.items, r.items);
        QCOMPARE(got.bytes, r.bytes);
        QCOMPARE(got.started, r.started);
        QVERIFY(!got.suspended);
    }

    void doneOrdinalsAreBatchedAndRead()
    {
        QTemporaryDir dir;
        TransferJobStore store(dir.path());
        store.save(sample(QStringLiteral("job1")));
        for (qint64 i = 0; i < 2000; i += 2)
            store.markDone(QStringLiteral("job1"), i);
        std::optional<QSet<qint64>> done;
        QObject ctx;
        store.loadDone(QStringLiteral("job1"), &ctx, [&](const QSet<qint64>& d) { done = d; });
        QTRY_VERIFY(done.has_value());
        QCOMPARE(done->size(), 1000);
        QVERIFY(done->contains(1998));
        QVERIFY(!done->contains(1));
    }

    void removeTakesEverything()
    {
        QTemporaryDir dir;
        TransferJobStore store(dir.path());
        store.save(sample(QStringLiteral("job2")));
        store.markDone(QStringLiteral("job2"), 7);
        QVERIFY(store.waitForWrites(5000));
        QFile plan(store.planPath(QStringLiteral("job2")));
        QVERIFY(plan.open(QIODevice::WriteOnly));
        plan.close();
        store.remove(QStringLiteral("job2"));
        QVERIFY(store.waitForWrites(5000));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }

    void pendingWritesSurviveTheStore()
    {
        QTemporaryDir dir;
        {
            TransferJobStore store(dir.path());
            store.save(sample(QStringLiteral("job3")));
            store.markDone(QStringLiteral("job3"), 42);   // buffered
        }   // the destructor writes it
        QFile f(dir.filePath(QStringLiteral("job3.done")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("42\n"));
    }
};

QTEST_GUILESS_MAIN(TestTransferJobStore)
#include "test_transfer_job_store.moc"
