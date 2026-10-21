// The version jobs through the OperationController against a versioned
// FakeObjectStore: DeleteVersionsJob (selected versions, everything of
// objects / folders, undelete) — what is planned from a fresh listing,
// what the preview says, locked versions, MFA delete, batches and singles,
// the journal, the verification.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/DeleteVersionsJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/RestoreVersionsJob.h"
#include "controllers/TransferService.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QFile>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {
const QString kBucket = QStringLiteral("bkt");
} // namespace

class TestVersionJobs : public QObject {
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

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    // As when the bucket is opened: its capabilities are probed first.
    // (Unknown never unlocks a destructive job.)
    bool probed()
    {
        if (!m_h->sessions->session(m_h->account.id))
            return false;
        if (!QTest::qWaitFor([this] { return m_caps->isLoaded(m_h->account.id); }, 10000))
            return false;
        m_caps->bucketOpened(m_h->account.id, kBucket);
        return QTest::qWaitFor([this] {
            return !m_caps->isProbing(m_h->account.id, kBucket) && m_caps->bucketSet(m_h->account.id, kBucket);
        }, 10000);
    }

    bool run(Job* job)
    {
        if (!probed())
            return false;
        if (auto* v = qobject_cast<DeleteVersionsJob*>(job))
            v->setRecheckDelayForTesting(10);
        if (auto* r = qobject_cast<RestoreVersionsJob*>(job))
            r->setRecheckDelayForTesting(10);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        return done.wait(30000) || done.size() == 1;
    }

    QStringList versionIds(const QString& key) const
    {
        QStringList ids;
        for (const FakeObjectStore::Version& v : m_objects->versions(kBucket, key))
            ids.append(v.deleteMarker ? QStringLiteral("marker:") + v.versionId : v.versionId);
        return ids;
    }

    QByteArray journal() const
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
        useProvider(QStringLiteral("aws"));   // BatchDelete is known there
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_objects->setVersioning(kBucket, QStringLiteral("Enabled"));
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
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            job->confirm();
        });
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

    void selectedVersionsGoForGood()
    {
        const QString k = QStringLiteral("docs/doc.txt");
        const QString v1 = m_objects->putObject(kBucket, k, "one");
        const QString v2 = m_objects->putObject(kBucket, k, "two");
        const QString v3 = m_objects->putObject(kBucket, k, "three");
        auto* job = DeleteVersionsJob::versions(m_h->account, kBucket,
                                                {{k, v1}, {k, v3}, {k, QStringLiteral("no-such-version")}},
                                                m_ops->planDir());
        QVERIFY(job->isDestructive());
        QVERIFY(job->requiredFeatures().contains(s3::Feature::ObjectVersionsListing));
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(versionIds(k), QStringList{v2});   // v2 is current now
        QCOMPARE(m_objects->object(kBucket, k)->data, QByteArray("two"));
        QCOMPARE(job->report().succeeded, 2);
        QCOMPARE(job->report().skipped, 1);   // the version that doesn't exist
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QVERIFY(job->usedBatches());

        QCOMPARE(m_previews.size(), 1);
        const Job::Preview& p = m_previews.first();
        QVERIFY(p.permanent);
        QCOMPARE(p.typeToConfirm, QStringLiteral("doc.txt"));   // always typed, also for two versions
        QCOMPARE(p.objects, 2);
        QVERIFY(p.warnings.join(u' ').contains(QLatin1String("current version")));
        QVERIFY(journal().contains(v3.toUtf8()));   // each version journaled with its id
    }

    void everythingOfAFolderGoes()
    {
        m_objects->putObject(kBucket, QStringLiteral("old/a.txt"), "a1");
        m_objects->putObject(kBucket, QStringLiteral("old/a.txt"), "a2");
        m_objects->putObject(kBucket, QStringLiteral("old/sub/b.txt"), "b");
        m_objects->deleteObject(kBucket, QStringLiteral("old/sub/b.txt"));
        m_objects->putObject(kBucket, QStringLiteral("older.txt"), "same prefix, other folder");
        m_objects->putObject(kBucket, QStringLiteral("keep/c.txt"), "c");
        auto* job = DeleteVersionsJob::everything(m_h->account, kBucket, {{QStringLiteral("old/"), true},
                                                                          {QStringLiteral("old/a.txt"), false}},
                                                  m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 4);   // a.txt covered by its folder: once
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(m_objects->versionedKeys(kBucket), (QStringList{QStringLiteral("keep/c.txt"), QStringLiteral("older.txt")}));
        const Job::Preview& p = m_previews.first();
        QCOMPARE(p.typeToConfirm, QStringLiteral("old"));
        QVERIFY(p.warnings.join(u' ').contains(QLatin1String("disappear completely")));
        QVERIFY(p.details.join(u' ').contains(QLatin1String("3 version(s) and 1 delete marker(s)")));
    }

    void undeleteBringsTheNewestVersionBack()
    {
        const QString a1 = m_objects->putObject(kBucket, QStringLiteral("f/a.txt"), "a");
        m_objects->deleteObject(kBucket, QStringLiteral("f/a.txt"));
        m_objects->putObject(kBucket, QStringLiteral("f/b.txt"), "b old");
        const QString b2 = m_objects->putObject(kBucket, QStringLiteral("f/b.txt"), "b new");
        m_objects->deleteObject(kBucket, QStringLiteral("f/b.txt"));
        m_objects->deleteObject(kBucket, QStringLiteral("f/b.txt"));   // two markers on top
        m_objects->putObject(kBucket, QStringLiteral("f/c.txt"), "alive");
        m_objects->deleteObject(kBucket, QStringLiteral("f/ghost.txt"));   // a marker with nothing below
        auto* job = DeleteVersionsJob::undelete(m_h->account, kBucket, {{QStringLiteral("f/"), true}}, m_ops->planDir());
        QVERIFY(!job->isDestructive());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 3);   // three markers removed
        QCOMPARE(job->report().skipped, 1);     // ghost: nothing to bring back
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("f/a.txt"))->data, QByteArray("a"));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("f/b.txt"))->data, QByteArray("b new"));
        QCOMPARE(versionIds(QStringLiteral("f/a.txt")), QStringList{a1});
        QCOMPARE(versionIds(QStringLiteral("f/b.txt")).first(), b2);
        QVERIFY(!m_objects->object(kBucket, QStringLiteral("f/ghost.txt")));   // left alone
        QCOMPARE(versionIds(QStringLiteral("f/ghost.txt")).size(), 1);
        const Job::Preview& p = m_previews.first();
        QVERIFY(!p.permanent);
        QVERIFY(p.typeToConfirm.isEmpty());
        QCOMPARE(p.objects, 2);   // objects that come back
        QVERIFY(journal().contains("undelete"));
    }

    void undeleteOnlyWhatWasDeletedAfterAMoment()
    {
        m_objects->putObject(kBucket, QStringLiteral("f/a.txt"), "a");
        m_objects->deleteObject(kBucket, QStringLiteral("f/a.txt"));
        auto* job = DeleteVersionsJob::undelete(m_h->account, kBucket, {{QStringLiteral("f/"), true}}, m_ops->planDir(),
                                                QDateTime::currentDateTimeUtc().addSecs(3600));
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);   // nothing to do
        QCOMPARE(job->report().succeeded, 0);
        QCOMPARE(job->report().skipped, 1);
        QVERIFY(m_previews.isEmpty());
        QVERIFY(!m_objects->object(kBucket, QStringLiteral("f/a.txt")));
    }

    void lockedVersionsAreReportedNotBypassed()
    {
        const QString v1 = m_objects->putObject(kBucket, QStringLiteral("l/x"), "locked");
        m_objects->putObject(kBucket, QStringLiteral("l/x"), "free");
        m_objects->lockVersion(kBucket, QStringLiteral("l/x"), v1);
        auto* job = DeleteVersionsJob::everything(m_h->account, kBucket, {{QStringLiteral("l/"), true}}, m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::PartlyFailed);
        QCOMPARE(job->report().failed, 1);
        QVERIFY(job->report().failures.first().contains(QLatin1String("object lock")));
        QCOMPARE(versionIds(QStringLiteral("l/x")), QStringList{v1});
        QVERIFY(job->report().verified);   // the failure is reported, the rest checked
        for (const RecordedRequest& r : m_h->server->requests())
            QVERIFY(r.header("x-amz-bypass-governance-retention").isNull());
    }

    void unprobedVersionListingRefusesDestructiveJobs()
    {
        const QString v1 = m_objects->putObject(kBucket, QStringLiteral("u.txt"), "u");
        auto* job = DeleteVersionsJob::versions(m_h->account, kBucket, {{QStringLiteral("u.txt"), v1}}, m_ops->planDir());
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY(m_caps->isLoaded(m_h->account.id));
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);   // the bucket not probed yet: Unknown
        QVERIFY(done.size() == 1 || done.wait(10000));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QCOMPARE(versionIds(QStringLiteral("u.txt")), QStringList{v1});
    }

    void mfaDeleteRefusesBeforeAnything()
    {
        const QString v1 = m_objects->putObject(kBucket, QStringLiteral("m.txt"), "m");
        m_objects->setMfaDelete(kBucket, true);
        auto* job = DeleteVersionsJob::versions(m_h->account, kBucket, {{QStringLiteral("m.txt"), v1}}, m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->errorText().contains(QLatin1String("MFA")));
        QCOMPARE(versionIds(QStringLiteral("m.txt")), QStringList{v1});
        QVERIFY(m_previews.isEmpty());
    }

    void restoreAnEarlierVersionWithItsTagsAndAcl()
    {
        m_objects->setDropTagsOnReplace(true);   // RGW: a copy onto itself loses the tags
        const QString k = QStringLiteral("r/page.html");
        const QString v1 = m_objects->putObject(kBucket, k, "<h1>old</h1>", QStringLiteral("text/html"));
        m_objects->setObjectHeader(kBucket, k, QStringLiteral("cache-control"), QStringLiteral("max-age=60"));
        m_objects->setObjectTags(kBucket, k, {{QStringLiteral("team"), QStringLiteral("web")}});
        m_objects->setObjectAcl(kBucket, k, QStringLiteral("public-read"));
        const QString v2 = m_objects->putObject(kBucket, k, "<h1>broken</h1>", QStringLiteral("text/plain"));
        auto* job = new RestoreVersionsJob(m_h->account, kBucket, {{k, v1}}, m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().failures.join(u'\n')));
        const auto now = m_objects->object(kBucket, k);
        QCOMPARE(now->data, QByteArray("<h1>old</h1>"));
        QCOMPARE(now->contentType, QStringLiteral("text/html"));
        QCOMPARE(now->headers.value(QStringLiteral("cache-control")), QStringLiteral("max-age=60"));
        QCOMPARE(now->tags, (s3::TagSet{{QStringLiteral("team"), QStringLiteral("web")}}));   // written back
        QCOMPARE(now->acl, QStringLiteral("public-read"));
        const QStringList ids = versionIds(k);
        QCOMPARE(ids.size(), 3);   // the restore is a new version; v2 stays
        QCOMPARE(ids.at(1), v2);
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("stay as earlier ones")));
        QVERIFY(!m_previews.first().permanent);
        // The copy came from v1, guarded by its ETag.
        bool fromV1 = false;
        for (const RecordedRequest& r : m_h->server->requests())
            fromV1 = fromV1 || (r.method == "PUT" && r.header("x-amz-copy-source").endsWith("?versionId=" + v1.toUtf8())
                                && !r.header("x-amz-copy-source-if-match").isEmpty());
        QVERIFY(fromV1);
    }

    void restoreSkipsWhatIsNoEarlierVersion()
    {
        const QString k = QStringLiteral("s/a.txt");
        m_objects->putObject(kBucket, k, "1");
        const QString current = m_objects->putObject(kBucket, k, "2");
        const QString marker = m_objects->deleteObject(kBucket, QStringLiteral("s/b.txt"));
        auto* job = new RestoreVersionsJob(m_h->account, kBucket,
                                           {{k, current}, {QStringLiteral("s/b.txt"), marker}, {k, QStringLiteral("gone")}},
                                           m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);   // nothing to do
        QCOMPARE(job->report().skipped, 3);
        const QString notes = job->report().notes.join(u'\n');
        QVERIFY(notes.contains(QLatin1String("current version already")));
        QVERIFY(notes.contains(QLatin1String("delete marker")));
        QVERIFY(notes.contains(QLatin1String("no longer exists")));
        QVERIFY(m_previews.isEmpty());
        QCOMPARE(versionIds(k).size(), 2);
    }

    void restoreWhereVersioningIsSuspendedIsPermanent()
    {
        const QString k = QStringLiteral("p.txt");
        const QString v1 = m_objects->putObject(kBucket, k, "keep");
        m_objects->setVersioning(kBucket, QStringLiteral("Suspended"));
        m_objects->putObject(kBucket, k, "null version");
        auto* job = new RestoreVersionsJob(m_h->account, kBucket, {{k, v1}}, m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(m_previews.first().permanent);
        QCOMPARE(m_objects->object(kBucket, k)->data, QByteArray("keep"));
        QCOMPARE(versionIds(k).size(), 2);   // the null version was replaced
    }

    void withoutBatchDeleteOneByOne()
    {
        useProvider(QStringLiteral("generic"));
        const QString v1 = m_objects->putObject(kBucket, QStringLiteral("s.txt"), "1");
        const QString v2 = m_objects->putObject(kBucket, QStringLiteral("s.txt"), "2");
        auto* job = DeleteVersionsJob::versions(m_h->account, kBucket, {{QStringLiteral("s.txt"), v1}}, m_ops->planDir());
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(!job->usedBatches());
        QCOMPARE(m_objects->count(QStringLiteral("DeleteObject")), 1);
        QCOMPARE(versionIds(QStringLiteral("s.txt")), QStringList{v2});
    }
};

QTEST_GUILESS_MAIN(TestVersionJobs)
#include "test_version_jobs.moc"
