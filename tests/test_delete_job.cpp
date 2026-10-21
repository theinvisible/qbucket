#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/DeleteJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/AccountHarness.h"
#include "support/FakeBucket.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;
using qbucket::testing::FakeBucket;

namespace {

QString k(int i)
{
    return QStringLiteral("photos/k%1").arg(i, 5, 10, QLatin1Char('0'));
}

} // namespace

class TestDeleteJob : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_bucket = FakeBucket();
        m_versioning.clear();
        m_ghosts.clear();
        m_lagging.clear();
        m_deleteDelayMs = 0;
        m_h->server->setFallbackHandler([this](const RecordedRequest& r) { return answer(r); });
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        useProvider(QStringLiteral("aws"));   // BatchDelete known
    }

    void cleanup()
    {
        m_ops.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_h.reset();
    }

    void aFolderIsDeletedCompletelyInBatches()
    {
        m_bucket.add(QStringLiteral("photos/"));
        for (int i = 0; i < 2500; ++i)
            m_bucket.add(k(i), 10);
        m_bucket.add(QStringLiteral("photosynthesis.txt"));   // same prefix, other folder
        m_bucket.add(QStringLiteral("other/keep"));
        m_bucket.sort();

        auto* job = make({{QStringLiteral("photos/"), true}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(10000));
        const Job::Preview& p = job->preview();
        QCOMPARE(p.objects, 2501);   // the marker counts
        QCOMPARE(p.bytes, 25000);
        QCOMPARE(p.samples.size(), DeleteJob::kSamples);
        QCOMPARE(p.typeToConfirm, QStringLiteral("photos"));
        QVERIFY(p.permanent);   // no versioning
        const QString planPath = m_ops->planDir() + u'/' + job->id() + QStringLiteral(".plan");
        QVERIFY(QFile::exists(planPath));
        QCOMPARE(m_bucket.objects().size(), 2503);   // planning deleted nothing

        job->confirm();
        QVERIFY(done.wait(30000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(job->report().succeeded, 2501);
        QVERIFY(job->usedBatches());
        QCOMPARE(m_bucket.deleteBatches, 3);
        QCOMPARE(m_bucket.keys(), (QStringList{QStringLiteral("other/keep"), QStringLiteral("photosynthesis.txt")}));

        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray journal = f.readAll();
        QCOMPARE(journal.count("\"message\":\"batch of"), 3);              // one line per request
        QCOMPARE(journal.count("\"op\":\"DeleteObjects\""), 3 + 2501);   // and one per object
        // The job is deleted after it finished — and its plan with it.
        QTRY_VERIFY(!QFile::exists(planPath));
    }

    void withoutConfirmedBatchDeleteOneByOne()
    {
        useProvider(QStringLiteral("generic"));
        for (int i = 0; i < 12; ++i)
            m_bucket.add(k(i));
        m_bucket.sort();
        auto* job = make({{QStringLiteral("photos/"), true}});
        runConfirmed(job);
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(!job->usedBatches());
        QCOMPARE(m_bucket.singleDeletes, 12);
        QCOMPARE(m_bucket.deleteBatches, 0);
        QVERIFY(m_bucket.objects().isEmpty());
    }

    void selectionsAreDeletedOnceAndMissingObjectsSkipped()
    {
        m_bucket.add(QStringLiteral("a.txt"), 5);
        m_bucket.add(QStringLiteral("d/x"), 7);
        m_bucket.add(QStringLiteral("d/y"), 7);
        m_bucket.add(QStringLiteral("keep.txt"));
        m_bucket.sort();
        auto* job = make({{QStringLiteral("a.txt"), false},
                          {QStringLiteral("d/"), true},
                          {QStringLiteral("d/x"), false},     // inside d/: once
                          {QStringLiteral("gone.txt"), false}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(10000));
        QCOMPARE(job->preview().objects, 3);
        QCOMPARE(job->preview().bytes, 19);
        QVERIFY(job->preview().details.join(u' ').contains(QLatin1String("1")));
        job->confirm();
        QVERIFY(done.wait(10000));
        QCOMPARE(job->report().succeeded, 3);
        QCOMPARE(job->report().skipped, 1);
        QVERIFY(job->report().verified);
        QCOMPARE(m_bucket.keys(), QStringList{QStringLiteral("keep.txt")});
    }

    void lockedKeysAreReportedAndCanBeRetried()
    {
        for (int i = 0; i < 20; ++i)
            m_bucket.add(k(i));
        m_bucket.sort();
        m_bucket.locked = {k(5), k(17)};
        auto* job = make({{QStringLiteral("photos/"), true}});
        runConfirmed(job);
        QCOMPARE(job->outcome(), Job::Outcome::PartlyFailed);
        QCOMPARE(job->report().succeeded, 18);
        QCOMPARE(job->report().failed, 2);
        QCOMPARE(job->failedKeys(), (QStringList{k(5), k(17)}));
        QVERIFY(job->report().failures.first().contains(QLatin1String("AccessDenied")));
        QVERIFY(job->report().verified);   // nothing else left

        // "Retry Failed" once the lock is gone.
        m_bucket.locked.clear();
        auto* retry = make({{k(5), false}, {k(17), false}});
        runConfirmed(retry);
        QCOMPARE(retry->outcome(), Job::Outcome::Succeeded);
        QVERIFY(m_bucket.objects().isEmpty());
    }

    void aVersionedBucketSaysItIsRecoverable()
    {
        m_versioning = QStringLiteral("Enabled");
        m_bucket.add(QStringLiteral("x"));
        auto* job = make({{QStringLiteral("x"), false}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        m_ops->start(job);
        QVERIFY(preview.wait(10000));
        QVERIFY(!job->preview().permanent);
        QVERIFY(job->preview().warnings.join(u' ').contains(QLatin1String("delete markers")));
        QVERIFY(job->preview().typeToConfirm.isEmpty());
        job->cancel();
        QVERIFY(m_bucket.contains(QStringLiteral("x")));
    }

    void cancellingStopsAfterWhatIsInFlight()
    {
        useProvider(QStringLiteral("generic"));   // one request per key
        for (int i = 0; i < 40; ++i)
            m_bucket.add(k(i));
        m_bucket.sort();
        m_deleteDelayMs = 50;
        auto* job = make({{QStringLiteral("photos/"), true}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(10000));
        job->confirm();
        QTRY_VERIFY_WITH_TIMEOUT(m_bucket.singleDeletes >= 6, 10000);
        job->cancel();
        QVERIFY(done.wait(10000));
        QCOMPARE(job->outcome(), Job::Outcome::Cancelled);
        const qint64 succeeded = job->report().succeeded;
        const int sent = m_bucket.singleDeletes;
        QTest::qWait(300);   // (the finished job is deleted meanwhile)
        QCOMPARE(m_bucket.singleDeletes, sent);   // nothing after the cancel
        QVERIFY(sent < 40);
        QCOMPARE(succeeded, 40 - m_bucket.objects().size());
    }

    void leftoversAreCheckedAgainBeforeTheyAreReported()
    {
        for (int i = 0; i < 5; ++i)
            m_bucket.add(k(i));
        m_bucket.sort();
        m_ghosts = {k(1)};        // "deleted", but stays
        m_lagging = {k(3)};       // listed once more after its delete
        auto* job = make({{QStringLiteral("photos/"), true}});
        job->setRecheckDelaysForTesting(10, 20);
        runConfirmed(job);
        QCOMPARE(job->outcome(), Job::Outcome::PartlyFailed);
        QVERIFY(!job->report().verified);
        QCOMPARE(job->failedKeys(), QStringList{k(1)});   // k(3) was only late
        QCOMPARE(job->report().succeeded, 4);
        QVERIFY(job->report().verification.contains(QLatin1String("1")));
    }

    void nothingToDeleteIsSaidSo()
    {
        auto* job = make({{QStringLiteral("empty/"), true}});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(10000));
        QVERIFY(preview.isEmpty());
        QCOMPARE(job->report().succeeded, 0);
    }

private:
    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    DeleteJob* make(const QList<DeleteJob::Item>& items)
    {
        return new DeleteJob(m_h->account, QStringLiteral("bkt"), items, m_ops->planDir());
    }

    void runConfirmed(Job* job)
    {
        QSignalSpy done(job, &Job::finished);
        connect(m_ops.get(), &OperationController::previewNeeded, job, [job](Job* j) {
            if (j == job)
                j->confirm();
        });
        m_ops->start(job);
        QVERIFY(done.wait(30000));
    }

    FakeResponse answer(const RecordedRequest& r)
    {
        if (r.method == "GET" && r.hasQueryItem("versioning"))
            return FakeResponse::ok("<VersioningConfiguration>" +
                                    (m_versioning.isEmpty() ? QByteArray()
                                                            : "<Status>" + m_versioning.toUtf8() + "</Status>")
                                    + "</VersioningConfiguration>");
        if (r.method == "GET" && r.hasQueryItem("object-lock"))
            return FakeResponse::s3Error(404, "ObjectLockConfigurationNotFoundError");
        if (r.method == "GET" && !r.query.isEmpty() && !r.hasQueryItem("list-type"))
            return FakeResponse::s3Error(501, "NotImplemented");
        if (r.method == "GET" && r.hasQueryItem("list-type")) {
            FakeResponse listing = m_bucket.list(r);
            // A lagging key reappears in the first listing after its delete.
            for (const QString& lag : std::as_const(m_lagSeen)) {
                listing.body.replace("</ListBucketResult>",
                                     "<Contents><Key>" + lag.toUtf8() + "</Key><Size>0</Size></Contents></ListBucketResult>");
            }
            m_lagSeen.clear();
            return listing;
        }
        if (r.method == "DELETE" || (r.method == "POST" && r.hasQueryItem("delete"))) {
            const QStringList before = m_bucket.keys();
            FakeResponse resp = m_bucket.handle(r);
            resp.delayMs = m_deleteDelayMs;
            for (const QString& g : std::as_const(m_ghosts)) {
                if (before.contains(g) && !m_bucket.contains(g)) {
                    m_bucket.add(g);
                    m_bucket.sort();
                }
            }
            for (const QString& lag : std::as_const(m_lagging)) {
                if (before.contains(lag) && !m_bucket.contains(lag))
                    m_lagSeen.insert(lag);
            }
            return resp;
        }
        return m_bucket.handle(r);
    }

    std::unique_ptr<AccountHarness> m_h;
    FakeBucket m_bucket;
    QString m_versioning;
    QSet<QString> m_ghosts;
    QSet<QString> m_lagging;
    QSet<QString> m_lagSeen;
    int m_deleteDelayMs = 0;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
};

QTEST_GUILESS_MAIN(TestDeleteJob)
#include "test_delete_job.moc"
