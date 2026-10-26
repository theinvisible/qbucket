// CreateBucketJob (names checked before sending, taken names refused,
// object lock and versioning, read-only accounts) and DeleteBucketJob
// (empty, refused while not empty, emptied first — versions, markers,
// uploads —, object lock with and without the bypass, something written
// between the two emptiness checks).

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CreateBucketJob.h"
#include "controllers/DeleteBucketJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QFile>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");

} // namespace

class TestBucketJobs : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
    QList<Job::Preview> m_previews;
    QStringList m_steps;
    bool m_bypass = false;
    std::function<void()> m_onConfirm;

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    void probe(const QString& bucket = kBucket)
    {
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY(m_caps->isLoaded(m_h->account.id));
        m_caps->bucketOpened(m_h->account.id, bucket);
        QTRY_VERIFY_WITH_TIMEOUT(!m_caps->isProbing(m_h->account.id, bucket) && m_caps->bucketSet(m_h->account.id, bucket),
                                 10000);
    }

    struct Outcome {
        Job::Outcome outcome = Job::Outcome::None;
        QString text;
        Job::Report report;
    };

    Outcome run(Job* job)
    {
        if (auto* create = qobject_cast<CreateBucketJob*>(job))
            create->setVerifyDelayForTesting(10);
        if (auto* del = qobject_cast<DeleteBucketJob*>(job))
            del->setDelaysForTesting(10, 0);
        std::optional<Outcome> out;
        connect(job, &Job::finished, this, [&out, job] {
            out = Outcome{job->outcome(), job->errorText() + QLatin1Char('|') + job->report().verification, job->report()};
        });
        recordSteps(job);
        m_ops->start(job);
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 30000))
            return {Job::Outcome::None, QStringLiteral("timeout"), {}};
        return *out;
    }

    // The job's steps as the status bar sees them, each once in a row.
    void recordSteps(Job* job)
    {
        m_steps.clear();
        connect(job, &Job::progressChanged, this, [this, job] {
            if (!job->step().isEmpty() && (m_steps.isEmpty() || m_steps.last() != job->step()))
                m_steps.append(job->step());
        });
    }
    // `expected` appear in m_steps in this order (others may come between).
    bool stepsInOrder(const QStringList& expected) const
    {
        qsizetype at = 0;
        for (const QString& e : expected) {
            while (at < m_steps.size() && !m_steps[at].startsWith(e))
                ++at;
            if (at == m_steps.size())
                return false;
            ++at;
        }
        return true;
    }

    QByteArray journalText()
    {
        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
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
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        m_previews.clear();
        m_bypass = false;
        m_onConfirm = {};
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            job->setBypassGovernance(m_bypass);
            if (m_onConfirm)
                m_onConfirm();
            job->confirm();
        });
        useProvider(QStringLiteral("aws"));
    }

    void cleanup()
    {
        m_ops.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void createABucket()
    {
        probe();
        const Outcome o = run(new CreateBucketJob(m_h->account, QStringLiteral("new-bucket"), {}));
        QCOMPARE(o.outcome, Job::Outcome::Succeeded);
        QVERIFY2(o.report.verified, qPrintable(o.text));
        QVERIFY(m_objects->hasBucket(QStringLiteral("new-bucket")));
        QCOMPARE(m_previews.size(), 1);
        QCOMPARE(m_previews.first().objects, qint64(-1));
        QVERIFY(journalText().contains("CreateBucket"));
    }

    void createWithObjectLockOrVersioning()
    {
        probe();
        CreateBucketJob::Options lock;
        lock.objectLock = true;
        Outcome o = run(new CreateBucketJob(m_h->account, QStringLiteral("locked-one"), lock));
        QCOMPARE(o.outcome, Job::Outcome::Succeeded);
        QVERIFY2(o.text.contains(QLatin1String("object lock is enabled")), qPrintable(o.text));
        QCOMPARE(m_objects->versioning(QStringLiteral("locked-one")), QStringLiteral("Enabled"));
        QCOMPARE(m_objects->count(QStringLiteral("PutBucketVersioning")), 0);   // came with the lock

        CreateBucketJob::Options versioned;
        versioned.versioning = true;
        o = run(new CreateBucketJob(m_h->account, QStringLiteral("versioned-one"), versioned));
        QCOMPARE(o.outcome, Job::Outcome::Succeeded);
        QVERIFY(o.report.verified);
        QCOMPARE(m_objects->versioning(QStringLiteral("versioned-one")), QStringLiteral("Enabled"));
    }

    void badOrTakenNamesAreRefusedBeforeSending()
    {
        probe();
        Outcome o = run(new CreateBucketJob(m_h->account, QStringLiteral("Bad_Name"), {}));
        QCOMPARE(o.outcome, Job::Outcome::Failed);
        QVERIFY(o.text.contains(QLatin1String("lower-case")));
        o = run(new CreateBucketJob(m_h->account, kBucket, {}));
        QCOMPARE(o.outcome, Job::Outcome::Failed);
        QVERIFY(o.text.contains(QLatin1String("exists already")));
        QCOMPARE(m_objects->count(QStringLiteral("CreateBucket")), 0);
        QVERIFY(m_previews.isEmpty());
    }

    void unconfirmedVersioningIsRefused()
    {
        useProvider(QStringLiteral("generic"));
        probe();
        CreateBucketJob::Options versioned;
        versioned.versioning = true;
        const Outcome o = run(new CreateBucketJob(m_h->account, QStringLiteral("versioned-one"), versioned));
        QCOMPARE(o.outcome, Job::Outcome::Failed);
        QVERIFY(o.text.contains(QLatin1String("not confirmed")));
        QVERIFY(!m_objects->hasBucket(QStringLiteral("versioned-one")));
    }

    void readOnlyAccountsCreateNothing()
    {
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        const Outcome o = run(new CreateBucketJob(ro, QStringLiteral("new-bucket"), {}));
        QCOMPARE(o.outcome, Job::Outcome::Refused);
        QVERIFY(!m_objects->hasBucket(QStringLiteral("new-bucket")));
        QCOMPARE(m_objects->count(QStringLiteral("HeadBucket")), 0);
    }

    void deleteAnEmptyBucket()
    {
        probe();
        const Outcome o = run(new DeleteBucketJob(m_h->account, kBucket, false, m_ops->planDir()));
        QCOMPARE(o.outcome, Job::Outcome::Succeeded);
        QVERIFY2(o.report.verified, qPrintable(o.text));
        QVERIFY(!m_objects->hasBucket(kBucket));
        QCOMPARE(m_previews.last().typeToConfirm, kBucket);   // always typed
        QVERIFY(m_previews.last().permanent);
        QVERIFY(!m_caps->bucketSet(m_h->account.id, kBucket).has_value());   // forgotten
        QVERIFY(journalText().contains("DeleteBucket"));
    }

    void aBucketThatIsntEmptyStaysUnlessEmptyingWasAsked()
    {
        m_objects->putObject(kBucket, QStringLiteral("a.txt"), "a");
        probe();
        const Outcome o = run(new DeleteBucketJob(m_h->account, kBucket, false, m_ops->planDir()));
        QCOMPARE(o.outcome, Job::Outcome::Failed);
        QVERIFY(o.text.contains(QLatin1String("isn't empty")));
        QVERIFY(m_objects->hasBucket(kBucket));
        QVERIFY(m_previews.isEmpty());
        QCOMPARE(m_objects->count(QStringLiteral("DeleteBucket")), 0);
    }

    void emptyThenDelete()
    {
        m_objects->setVersioning(kBucket, QStringLiteral("Enabled"));
        m_objects->putObject(kBucket, QStringLiteral("a.txt"), "1");
        m_objects->putObject(kBucket, QStringLiteral("a.txt"), "2");
        m_objects->putObject(kBucket, QStringLiteral("dir/b.txt"), "b");
        m_objects->deleteObject(kBucket, QStringLiteral("dir/b.txt"));
        probe();
        // An unfinished upload, over HTTP.
        s3::Session* session = *m_h->sessions->session(m_h->account.id);
        s3::CreateMultipartUploadParams mp;
        mp.bucket = kBucket;
        mp.key = QStringLiteral("big.bin");
        bool started = false;
        session->createMultipartUpload(mp, this, [&](const s3::Result<s3::CreateMultipartUploadResult>& r) { started = bool(r); });
        QTRY_VERIFY(started);

        const Outcome o = run(new DeleteBucketJob(m_h->account, kBucket, true, m_ops->planDir()));
        QCOMPARE(o.outcome, Job::Outcome::Succeeded);
        QVERIFY2(o.report.verified, qPrintable(o.text));
        QVERIFY(!m_objects->hasBucket(kBucket));
        const Job::Preview& p = m_previews.last();
        QCOMPARE(p.objects, qint64(4));   // three versions, one marker
        QVERIFY(p.amount.contains(QLatin1String("unfinished upload")));
        QVERIFY(p.permanent);
        QCOMPARE(p.typeToConfirm, kBucket);
        QVERIFY(!p.bypassOffer);
        QCOMPARE(o.report.succeeded, qint64(4 + 1 + 1));   // versions, the upload, the bucket
        // The status bar can say what happens at every stage.
        QVERIFY2(stepsInOrder({QStringLiteral("listing the bucket"), QStringLiteral("listing unfinished uploads"),
                               QStringLiteral("deleting the contents"), QStringLiteral("aborting unfinished uploads"),
                               QStringLiteral("checking that the bucket is empty"),
                               QStringLiteral("checking that the bucket is still empty"),
                               QStringLiteral("deleting the bucket"), QStringLiteral("checking that the bucket is gone")}),
                 qPrintable(m_steps.join(u'|')));
        const QByteArray journal = journalText();
        QVERIFY(journal.contains("AbortMultipartUpload"));
        QVERIFY(journal.contains("DeleteBucket"));
    }

    void objectLockHoldsUnlessBypassed()
    {
        m_objects->setObjectLockEnabled(kBucket);
        const QString v = m_objects->putObject(kBucket, QStringLiteral("kept.txt"), "x");
        m_objects->setRetention(kBucket, QStringLiteral("kept.txt"), v, s3::ObjectLockMode::Governance,
                                QDateTime::currentDateTimeUtc().addDays(1));
        probe();
        Outcome o = run(new DeleteBucketJob(m_h->account, kBucket, true, m_ops->planDir()));
        QVERIFY(o.outcome == Job::Outcome::Failed || o.outcome == Job::Outcome::PartlyFailed);
        QVERIFY(m_previews.last().bypassOffer);
        QVERIFY(m_objects->hasBucket(kBucket));
        QCOMPARE(m_objects->count(QStringLiteral("DeleteBucket")), 0);   // never tried with a version left

        m_bypass = true;
        o = run(new DeleteBucketJob(m_h->account, kBucket, true, m_ops->planDir()));
        QCOMPARE(o.outcome, Job::Outcome::Succeeded);
        QVERIFY(!m_objects->hasBucket(kBucket));
        QVERIFY(journalText().contains("\"bypassGovernance\":true"));
    }

    void aLegalHoldKeepsTheBucket()
    {
        m_objects->setObjectLockEnabled(kBucket);
        const QString v = m_objects->putObject(kBucket, QStringLiteral("held.txt"), "x");
        m_objects->setLegalHold(kBucket, QStringLiteral("held.txt"), v, true);
        probe();
        m_bypass = true;
        const Outcome o = run(new DeleteBucketJob(m_h->account, kBucket, true, m_ops->planDir()));
        QVERIFY(o.outcome == Job::Outcome::Failed || o.outcome == Job::Outcome::PartlyFailed);
        QVERIFY2(o.text.contains(QLatin1String("stays")), qPrintable(o.text));
        QVERIFY(m_objects->hasBucket(kBucket));
        QCOMPARE(o.report.failed, qint64(1));
    }

    // The server's settle time (Hetzner: 20 s) is the longest quiet stretch:
    // the step says it is a wait, not a hang.
    void theSettleWaitIsAStepOfItsOwn()
    {
        probe();
        auto* job = new DeleteBucketJob(m_h->account, kBucket, false, m_ops->planDir());
        job->setDelaysForTesting(10, 1000);
        bool done = false;
        connect(job, &Job::finished, this, [&done] { done = true; });
        recordSteps(job);
        m_ops->start(job);
        QVERIFY(QTest::qWaitFor([&] { return done; }, 30000));
        QVERIFY(!m_objects->hasBucket(kBucket));
        QVERIFY2(stepsInOrder({QStringLiteral("checking that the bucket is empty"), QStringLiteral("waiting 1 s"),
                               QStringLiteral("checking that the bucket is still empty"),
                               QStringLiteral("deleting the bucket")}),
                 qPrintable(m_steps.join(u'|')));
    }

    // Written between "empty" and "still empty": the bucket stays.
    void somethingWrittenMeanwhileKeepsTheBucket()
    {
        probe();
        auto* job = new DeleteBucketJob(m_h->account, kBucket, false, m_ops->planDir());
        job->setDelaysForTesting(10, 1500);
        std::optional<Outcome> out;
        connect(job, &Job::finished, this, [&out, job] {
            out = Outcome{job->outcome(), job->errorText(), job->report()};
        });
        m_onConfirm = [this] {
            QTimer::singleShot(600, this, [this] { m_objects->putObject(kBucket, QStringLiteral("late.txt"), "x"); });
        };
        m_ops->start(job);
        QVERIFY(QTest::qWaitFor([&] { return out.has_value(); }, 30000));
        QCOMPARE(out->outcome, Job::Outcome::Failed);
        QVERIFY2(out->text.contains(QLatin1String("meanwhile")), qPrintable(out->text));
        QVERIFY(m_objects->hasBucket(kBucket));
        QCOMPARE(m_objects->count(QStringLiteral("DeleteBucket")), 0);
    }
};

QTEST_GUILESS_MAIN(TestBucketJobs)
#include "test_bucket_jobs.moc"
