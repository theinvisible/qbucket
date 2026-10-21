// CopyJob through the OperationController against FakeObjectStore: copy and
// move of objects and folders, also into another bucket; the conflict merge
// over paged listings, the policies, duplicates, refusals, a source that
// changed after the plan, objects above 5 GiB, the gate across buckets.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CopyJob.h"
#include "controllers/Journal.h"
#include "controllers/ObjectCopyRunner.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {
const QString kSrc = QStringLiteral("bkt");
const QString kDst = QStringLiteral("archive");
} // namespace

class TestCopyJob : public QObject {
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
    QList<Job::Preview> m_previews;
    std::function<void(Job*)> m_beforeConfirm;
    Job::ConflictPolicy m_policy = Job::ConflictPolicy::Skip;
    bool m_usePolicy = false;

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    CopyJob* make(const QList<CopyJob::Item>& items, const QString& base, const QString& dstBucket,
                  const QString& dstPrefix, CopyJob::Mode mode)
    {
        auto* job = new CopyJob(m_h->account, kSrc, items, base, dstBucket, dstPrefix, mode, m_ops->planDir());
        job->setRecheckDelayForTesting(10);
        return job;
    }

    // Starts and waits for the end; the preview is confirmed (after
    // m_beforeConfirm, with m_policy if set).
    bool run(Job* job)
    {
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        return done.wait(30000) || done.size() == 1;
    }

    QStringList keys(const QString& bucket) const { return m_objects->keys(bucket); }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        useProvider(QStringLiteral("aws"));   // CrossBucketCopy is known there
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kSrc);
        m_objects->createBucket(kDst);
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
        m_previews.clear();
        m_beforeConfirm = nullptr;
        m_usePolicy = false;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            if (m_beforeConfirm)
                m_beforeConfirm(job);
            if (m_usePolicy)
                job->setConflictPolicy(m_policy);
            job->confirm();
        });
        for (const char* k : {"photos/", "photos/a.jpg", "photos/b.jpg", "photos/2025/c.jpg", "notes.txt"})
            m_objects->putObject(kSrc, QString::fromLatin1(k), QByteArray("data of ") + k);
    }

    void cleanup()
    {
        m_ops.reset();
        m_transfers.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void copiesAFolderIntoAnotherBucket()
    {
        auto* job = make({{QStringLiteral("photos/"), true}}, QString(), kDst, QStringLiteral("old/"), CopyJob::Mode::Copy);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 4);
        QVERIFY(job->report().verified);
        QCOMPARE(keys(kDst), (QStringList{QStringLiteral("old/photos/"), QStringLiteral("old/photos/2025/c.jpg"),
                                          QStringLiteral("old/photos/a.jpg"), QStringLiteral("old/photos/b.jpg")}));
        QCOMPARE(m_objects->object(kDst, QStringLiteral("old/photos/2025/c.jpg"))->data, QByteArray("data of photos/2025/c.jpg"));
        QCOMPARE(keys(kSrc).size(), 5);   // a copy leaves the originals
        QCOMPARE(m_previews.size(), 1);
        QCOMPARE(m_previews.first().objects, 4);
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("s3://archive/old/")));
        QVERIFY(!job->isDestructive());
        QVERIFY(job->buckets().contains(kDst));
        QVERIFY(job->requiredFeatures().contains(s3::Feature::CrossBucketCopy));
    }

    void movesAndDeletesOnlyVerifiedCopies()
    {
        // One copy fails: its original stays, the others are moved.
        m_objects->addFault({QStringLiteral("CopyObject"), 0, 1, 1, false, FakeResponse::s3Error(500, "InternalError")});
        auto* job = make({{QStringLiteral("photos/"), true}}, QString(), kSrc, QStringLiteral("moved/"), CopyJob::Mode::Move);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::PartlyFailed);
        QCOMPARE(job->report().succeeded, 3);
        QCOMPARE(job->failedKeys().size(), 1);
        const QString failed = job->failedKeys().first();
        QVERIFY(keys(kSrc).contains(failed));                       // the original of the failed copy
        QVERIFY(!keys(kSrc).contains(QStringLiteral("photos/b.jpg")) || failed == QStringLiteral("photos/b.jpg"));
        QVERIFY(job->report().failures.first().contains(QLatin1String("untouched")));
        QVERIFY(job->report().verified);                            // only the failed one is left
        QVERIFY(job->isDestructive());
        QVERIFY(m_previews.first().permanent);                      // no versions: deletes for good
    }

    void conflictPolicies_data()
    {
        QTest::addColumn<int>("policy");
        QTest::addColumn<QString>("expectKey");
        QTest::addColumn<QByteArray>("expectData");
        QTest::newRow("skip") << int(Job::ConflictPolicy::Skip) << QStringLiteral("old/notes.txt") << QByteArray("older");
        QTest::newRow("overwrite") << int(Job::ConflictPolicy::Overwrite) << QStringLiteral("old/notes.txt")
                                   << QByteArray("data of notes.txt");
        QTest::newRow("keep both") << int(Job::ConflictPolicy::KeepBoth) << QStringLiteral("old/notes (1).txt")
                                   << QByteArray("data of notes.txt");
    }

    void conflictPolicies()
    {
        QFETCH(int, policy);
        QFETCH(QString, expectKey);
        QFETCH(QByteArray, expectData);
        m_objects->putObject(kDst, QStringLiteral("old/notes.txt"), "older");
        m_usePolicy = true;
        m_policy = Job::ConflictPolicy(policy);
        auto* job = make({{QStringLiteral("notes.txt"), false}}, QString(), kDst, QStringLiteral("old/"), CopyJob::Mode::Copy);
        QVERIFY(run(job));
        QCOMPARE(m_previews.first().conflicts, 1);
        QVERIFY(m_previews.first().conflictSamples.first().contains(QLatin1String("old/notes.txt")));
        QCOMPARE(m_objects->object(kDst, expectKey)->data, expectData);
        if (m_policy == Job::ConflictPolicy::Skip) {
            QCOMPARE(job->report().skipped, 1);
            QCOMPARE(m_objects->object(kDst, QStringLiteral("old/notes.txt"))->data, QByteArray("older"));
        }
        if (m_policy == Job::ConflictPolicy::KeepBoth)
            QCOMPARE(m_objects->object(kDst, QStringLiteral("old/notes.txt"))->data, QByteArray("older"));
    }

    // The merge of source and target listings over several pages.
    void conflictsAreFoundAcrossPages()
    {
        for (int i = 0; i < 2500; ++i) {
            const QString k = QStringLiteral("big/f%1").arg(i, 4, 10, QLatin1Char('0'));
            m_objects->putObject(kSrc, k, "x");
            if (i % 3 == 0)
                m_objects->putObject(kDst, QStringLiteral("copy/") + k, "y");
        }
        m_beforeConfirm = [](Job* job) { job->cancel(); };   // only the plan matters
        auto* job = make({{QStringLiteral("big/"), true}}, QString(), kDst, QStringLiteral("copy/"), CopyJob::Mode::Copy);
        QVERIFY(run(job));
        QCOMPARE(m_previews.first().objects, 2500);
        QCOMPARE(m_previews.first().conflicts, 834);
        QVERIFY(m_objects->count(QStringLiteral("ListObjectsV2")) >= 6);   // paged on both sides
    }

    void refusals()
    {
        // A folder into itself.
        auto* into = make({{QStringLiteral("photos/"), true}}, QString(), kSrc, QStringLiteral("photos/2025/"), CopyJob::Mode::Copy);
        QVERIFY(run(into));
        QCOMPARE(into->outcome(), Job::Outcome::Failed);
        QVERIFY(into->errorText().contains(QLatin1String("into itself")));
        // A move onto its own place.
        auto* same = make({{QStringLiteral("photos/a.jpg"), false}}, QStringLiteral("photos/"), kSrc, QStringLiteral("photos/"),
                          CopyJob::Mode::Move);
        QVERIFY(run(same));
        QCOMPARE(same->outcome(), Job::Outcome::Failed);
        QCOMPARE(keys(kSrc).size(), 5);
    }

    // A copy onto its own place makes a second one with a new name.
    void duplicatesGetANewName()
    {
        auto* job = make({{QStringLiteral("photos/a.jpg"), false}}, QStringLiteral("photos/"), kSrc, QStringLiteral("photos/"),
                         CopyJob::Mode::Copy);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_previews.first().policies,
                 (QList<Job::ConflictPolicy>{Job::ConflictPolicy::KeepBoth, Job::ConflictPolicy::Skip}));
        QCOMPARE(m_objects->object(kSrc, QStringLiteral("photos/a (1).jpg"))->data, QByteArray("data of photos/a.jpg"));
        QVERIFY(!job->isDestructive());
    }

    // The source changed between plan and copy: a move leaves it alone.
    void aSourceChangedAfterThePlanIsLeftAlone()
    {
        m_beforeConfirm = [this](Job*) { m_objects->putObject(kSrc, QStringLiteral("notes.txt"), "rewritten"); };
        auto* job = make({{QStringLiteral("notes.txt"), false}}, QString(), kSrc, QStringLiteral("moved/"), CopyJob::Mode::Move);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->report().failures.first().contains(QLatin1String("changed since it was planned")));
        QCOMPARE(m_objects->object(kSrc, QStringLiteral("notes.txt"))->data, QByteArray("rewritten"));
        QVERIFY(!m_objects->object(kDst, QStringLiteral("moved/notes.txt")));
    }

    void objectsAboveFiveGiBAreCopiedInParts()
    {
        m_objects->putSyntheticObject(kSrc, QStringLiteral("disk/huge.img"), ObjectCopyRunner::kCopyLimit + 1);
        auto* job = make({{QStringLiteral("disk/"), true}}, QString(), kDst, QString(), CopyJob::Mode::Move);
        QVERIFY(run(job));
        if (job->outcome() != Job::Outcome::Succeeded)
            QFAIL(qPrintable(job->report().failures.join(u'|') + job->errorText()));
        QVERIFY(job->requiredFeatures().contains(s3::Feature::MultipartUpload));
        QCOMPARE(m_objects->object(kDst, QStringLiteral("disk/huge.img"))->size, ObjectCopyRunner::kCopyLimit + 1);
        QVERIFY(!m_objects->object(kSrc, QStringLiteral("disk/huge.img")));
        QVERIFY(m_objects->count(QStringLiteral("UploadPartCopy")) > 1);
        QVERIFY(m_objects->uploadIds(kDst).isEmpty());
    }

    // A generic server: copying between buckets isn't confirmed — a copy
    // may try, a move (destructive) is refused.
    void unconfirmedCrossBucketCopyNeverMoves()
    {
        useProvider(QStringLiteral("generic"));
        auto* move = make({{QStringLiteral("notes.txt"), false}}, QString(), kDst, QString(), CopyJob::Mode::Move);
        QVERIFY(run(move));
        QCOMPARE(move->outcome(), Job::Outcome::Refused);
        QVERIFY(keys(kSrc).contains(QStringLiteral("notes.txt")));
        auto* copy = make({{QStringLiteral("notes.txt"), false}}, QString(), kDst, QString(), CopyJob::Mode::Copy);
        QVERIFY(run(copy));
        QCOMPARE(copy->outcome(), Job::Outcome::Succeeded);
    }
};

QTEST_GUILESS_MAIN(TestCopyJob)
#include "test_copy_job.moc"
