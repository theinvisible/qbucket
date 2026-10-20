// DownloadJob through the OperationController against FakeObjectStore:
// folders, local conflicts and the policies, read-only accounts, objects
// that change after the plan, names that must not escape the target.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/DownloadJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/PlanFile.h"
#include "controllers/TransferService.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestDownloadJob : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<TransferService> m_transfers;
    std::unique_ptr<OperationController> m_ops;
    QString m_target;

    void setAccount(Environment env, bool readOnly)
    {
        Account a = m_h->account;
        a.environment = env;
        a.readOnly = readOnly;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    DownloadJob* make(const QList<DownloadJob::Item>& items, bool alwaysConfirm = true)
    {
        DownloadJob::Options o;
        o.alwaysConfirm = alwaysConfirm;
        return new DownloadJob(m_h->account, kBucket, items, m_target, m_ops->planDir(), o);
    }

    bool runConfirmed(Job* job, Job::ConflictPolicy policy = Job::ConflictPolicy::Skip)
    {
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        if (!preview.wait(20000))
            return false;
        job->setConflictPolicy(policy);
        job->confirm();
        return done.wait(60000) || done.size() == 1;
    }

    QString local(const QString& rel) const { return m_target + u'/' + rel; }

    void writeLocal(const QString& rel, const QByteArray& data)
    {
        QDir().mkpath(QFileInfo(local(rel)).absolutePath());
        QFile f(local(rel));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_transfers = std::make_unique<TransferService>(m_h->path(QStringLiteral("transfers")), nullptr);
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        m_ops->setTransfers(m_transfers.get());
        m_target = m_h->path(QStringLiteral("downloads"));
        QDir().mkpath(m_target);
    }

    void cleanup()
    {
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        m_ops.reset();
        m_transfers.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void aFolderArrivesWithItsStructure()
    {
        m_objects->putObject(kBucket, QStringLiteral("photos/"), QByteArray());
        m_objects->putObject(kBucket, QStringLiteral("photos/a.jpg"), "aaa");
        m_objects->putObject(kBucket, QStringLiteral("photos/2026/ä b.jpg"), "bbb");
        m_objects->putObject(kBucket, QStringLiteral("photos/empty/"), QByteArray());
        m_objects->putObject(kBucket, QStringLiteral("photosynthesis.txt"), "not part of it");
        auto* job = make({{QStringLiteral("photos/"), true, {}}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        QCOMPARE(job->preview().objects, 4);
        QCOMPARE(job->preview().bytes, 6);
        QCOMPARE(job->preview().conflicts, 0);
        QVERIFY(!job->isMutating());
        job->confirm();
        QVERIFY(done.wait(30000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 4);
        QCOMPARE(job->md5Verified(), 2);
        QCOMPARE(readFile(local(QStringLiteral("photos/a.jpg"))), QByteArray("aaa"));
        QCOMPARE(readFile(local(QStringLiteral("photos/2026/ä b.jpg"))), QByteArray("bbb"));
        QVERIFY(QFileInfo(local(QStringLiteral("photos/empty"))).isDir());
        QVERIFY(!QFile::exists(local(QStringLiteral("photosynthesis.txt"))));
        const QDateTime server = m_objects->object(kBucket, QStringLiteral("photos/a.jpg"))->lastModified;
        QVERIFY(std::abs(QFileInfo(local(QStringLiteral("photos/a.jpg"))).lastModified(QTimeZone::UTC).secsTo(server)) <= 1);
        QVERIFY(QDir(m_target).entryList({QStringLiteral("*.qbpart")}, QDir::Files).isEmpty());
    }

    void oneObjectStraightIntoTheDownloadFolder()
    {
        setAccount(Environment::Development, false);
        m_objects->putObject(kBucket, QStringLiteral("docs/report.pdf"), "pdf");
        auto* job = make({{QStringLiteral("docs/report.pdf"), false, {}}}, false);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(20000));
        QVERIFY(preview.isEmpty());
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(readFile(local(QStringLiteral("report.pdf"))), QByteArray("pdf"));
    }

    void readOnlyAccountsMayDownload()
    {
        setAccount(Environment::Production, true);
        m_objects->putObject(kBucket, QStringLiteral("a.txt"), "a");
        auto* job = make({{QStringLiteral("a.txt"), false, {}}});
        QVERIFY(runConfirmed(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(readFile(local(QStringLiteral("a.txt"))), QByteArray("a"));
    }

    void localConflictsFollowThePolicy()
    {
        m_objects->putObject(kBucket, QStringLiteral("d/x.txt"), "server x");
        m_objects->putObject(kBucket, QStringLiteral("d/y.txt"), "server y");
        writeLocal(QStringLiteral("d/x.txt"), "local x");

        auto* skip = make({{QStringLiteral("d/"), true, {}}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(skip, &Job::finished);
        m_ops->start(skip);
        QVERIFY(preview.wait(20000));
        QCOMPARE(skip->preview().conflicts, 1);
        QVERIFY(skip->preview().conflictSamples.first().startsWith(QLatin1String("d/x.txt")));
        skip->confirm();
        QVERIFY(done.wait(20000));
        QCOMPARE(skip->report().skipped, 1);
        QCOMPARE(readFile(local(QStringLiteral("d/x.txt"))), QByteArray("local x"));
        QCOMPARE(readFile(local(QStringLiteral("d/y.txt"))), QByteArray("server y"));

        auto* both = make({{QStringLiteral("d/x.txt"), false, QStringLiteral("d/x.txt")}});
        QVERIFY(runConfirmed(both, Job::ConflictPolicy::KeepBoth));
        QCOMPARE(readFile(local(QStringLiteral("d/x.txt"))), QByteArray("local x"));
        QCOMPARE(readFile(local(QStringLiteral("d/x (1).txt"))), QByteArray("server x"));

        auto* over = make({{QStringLiteral("d/x.txt"), false, QStringLiteral("d/x.txt")}});
        QVERIFY(runConfirmed(over, Job::ConflictPolicy::Overwrite));
        QCOMPARE(over->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(readFile(local(QStringLiteral("d/x.txt"))), QByteArray("server x"));
    }

    void anObjectChangedAfterThePlanFailsAndCanBeRetried()
    {
        m_objects->putObject(kBucket, QStringLiteral("live.txt"), "v1");
        auto* job = make({{QStringLiteral("live.txt"), false, {}}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        m_objects->putObject(kBucket, QStringLiteral("live.txt"), "v2, longer");
        job->confirm();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->report().failures.first().contains(QLatin1String("live.txt")));
        QVERIFY(!QFile::exists(local(QStringLiteral("live.txt"))));
        QCOMPARE(job->failedItems().size(), 1);

        auto* retry = new DownloadJob(m_h->account, kBucket, job->failedItems(), m_target, m_ops->planDir(), {});
        QVERIFY(runConfirmed(retry));
        QCOMPARE(retry->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(readFile(local(QStringLiteral("live.txt"))), QByteArray("v2, longer"));
    }

    void namesNeverEscapeTheTarget()
    {
        m_objects->putObject(kBucket, QStringLiteral("box/../../escape.txt"), "evil");
        m_objects->putObject(kBucket, QStringLiteral("box/fine.txt"), "fine");
        m_objects->putObject(kBucket, QStringLiteral("box/a"), "file a");
        m_objects->putObject(kBucket, QStringLiteral("box/a/b"), "under a");   // "a" can't be both here
        auto* job = make({{QStringLiteral("box/"), true, {}}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        QCOMPARE(job->preview().objects, 2);   // fine.txt and one of a / a/b
        QVERIFY(!job->preview().warnings.isEmpty());
        job->confirm();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().skipped, 2);
        QCOMPARE(readFile(local(QStringLiteral("box/fine.txt"))), QByteArray("fine"));
        QVERIFY(!QFile::exists(m_h->path(QStringLiteral("escape.txt"))));
        QVERIFY(!QFile::exists(m_target + QStringLiteral("/escape.txt")));
    }

    void theLocalCheckMapsAndFindsCollisions()
    {
        // Windows rules on any system: names replaced, case-insensitive collisions.
        const QString raw = m_h->path(QStringLiteral("raw.plan"));
        const QString out = m_h->path(QStringLiteral("final.plan"));
        QFile f(raw);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(PlanFile::encodeRecord({QStringLiteral("k1"), QStringLiteral("dir/Report?.txt"), QStringLiteral("5"),
                                        QStringLiteral("\"e\""), QStringLiteral("0"), QStringLiteral("STANDARD")}));
        f.write(PlanFile::encodeRecord({QStringLiteral("k2"), QStringLiteral("DIR/report?.TXT"), QStringLiteral("5"),
                                        QStringLiteral("\"e\""), QStringLiteral("0"), QString()}));
        f.write(PlanFile::encodeRecord({QStringLiteral("k3"), QStringLiteral("cold.bin"), QStringLiteral("9"),
                                        QStringLiteral("\"e\""), QStringLiteral("0"), QStringLiteral("GLACIER")}));
        f.write(PlanFile::encodeRecord({QStringLiteral("k4"), QStringLiteral("aux.log"), QStringLiteral("1"),
                                        QStringLiteral("\"e\""), QStringLiteral("0"), QString()}));
        f.close();
        std::atomic<bool> cancel{false};
        const DownloadJob::LocalCheck c =
            DownloadJob::checkLocal(raw, out, m_target, LocalPathMapper::Rules::Windows, cancel);
        QVERIFY(c.error.isEmpty());
        QCOMPARE(c.files, 2);
        QCOMPARE(c.skipped, 2);   // the case-insensitive twin, the archived one
        QCOMPARE(c.renamed, 2);
        QFile finalPlan(out);
        QVERIFY(finalPlan.open(QIODevice::ReadOnly));
        const QStringList first = PlanFile::decodeRecord(finalPlan.readLine());
        QCOMPARE(first.value(1), QStringLiteral("dir/Report_.txt"));
        const QStringList second = PlanFile::decodeRecord(finalPlan.readLine());
        QCOMPARE(second.value(1), QStringLiteral("_aux.log"));
    }

    void aLargeObjectInRangesIsVerified()
    {
        const QByteArray big(65 * 1024 * 1024 + 3, 'q');
        m_objects->putObject(kBucket, QStringLiteral("big.bin"), big);
        auto* job = make({{QStringLiteral("big.bin"), false, {}}});
        QVERIFY(runConfirmed(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->md5Verified(), 1);
        QCOMPARE(QFileInfo(local(QStringLiteral("big.bin"))).size(), big.size());
        QVERIFY(m_objects->count(QStringLiteral("GetObject")) >= 9);
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        QVERIFY(m_transfers->resumeStore()->ids().isEmpty());
    }

    void cancelRemovesWhatWasHalfDone()
    {
        for (int i = 0; i < 30; ++i)
            m_objects->putObject(kBucket, QStringLiteral("many/f%1").arg(i), QByteArray(1000, 'x'));
        m_objects->setDelay(QStringLiteral("GetObject"), 200);
        auto* job = make({{QStringLiteral("many/"), true, {}}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        job->confirm();
        QTRY_VERIFY(m_objects->count(QStringLiteral("GetObject")) >= 2);
        job->cancel();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Cancelled);
        QTRY_VERIFY(QDir(m_target + QStringLiteral("/many")).entryList({QStringLiteral("*.qbpart")}, QDir::Files).isEmpty());
        QTRY_COMPARE(m_transfers->limiter(m_h->account.id)->inFlight(), 0);
    }
};

QTEST_GUILESS_MAIN(TestDownloadJob)
#include "test_download_job.moc"
