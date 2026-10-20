#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/RenameJob.h"
#include "support/AccountHarness.h"
#include "support/FakeBucket.h"

#include <QFile>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;
using qbucket::testing::FakeBucket;
using qbucket::testing::FakeObject;

class TestRenameJob : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_bucket = FakeBucket();
        m_changeAfterPlan.clear();
        m_h->server->setFallbackHandler([this](const RecordedRequest& r) {
            if (r.method == "GET" && !r.query.isEmpty() && !r.hasQueryItem("list-type"))
                return r.hasQueryItem("versioning") ? FakeResponse::ok("<VersioningConfiguration/>")
                                                    : FakeResponse::s3Error(501, "NotImplemented");
            return m_bucket.handle(r);
        });
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            // Something changes on the server between preview and confirm.
            for (const QString& k : std::as_const(m_changeAfterPlan)) {
                if (FakeObject* o = m_bucket.find(k))
                    o->etag = "\"changed\"";
            }
            job->confirm();
        });
    }

    void cleanup()
    {
        m_ops.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_h.reset();
        m_previews.clear();
    }

    void renamesAnObjectKeepingItsClass()
    {
        m_bucket.add(QStringLiteral("docs/a.txt"), 42, "GLACIER_IR");
        m_bucket.add(QStringLiteral("docs/other.txt"), 1);
        m_bucket.sort();
        Job* job = run(QStringLiteral("docs/a.txt"), false, QStringLiteral("b.txt"));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(m_previews.size(), 1);
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("docs/b.txt")));
        QVERIFY(!m_bucket.contains(QStringLiteral("docs/a.txt")));
        QVERIFY(m_bucket.contains(QStringLiteral("docs/b.txt")));
        QCOMPARE(m_bucket.find(QStringLiteral("docs/b.txt"))->storageClass, QByteArray("GLACIER_IR"));
        QCOMPARE(m_bucket.find(QStringLiteral("docs/b.txt"))->size, 42);

        // Copy with the planned ETag, before the delete — in this order.
        QList<QByteArray> methods;
        RecordedRequest copy;
        for (const RecordedRequest& r : m_h->server->requests()) {
            if (r.method == "PUT" || r.method == "DELETE")
                methods.append(r.method);
            if (r.method == "PUT")
                copy = r;
        }
        QCOMPARE(methods, (QList<QByteArray>{"PUT", "DELETE"}));
        QCOMPARE(copy.header("x-amz-copy-source-if-match"), QByteArray("\"0\""));
        QCOMPARE(copy.header("x-amz-copy-source"), QByteArray("/bkt/docs/a.txt"));

        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray journal = f.readAll();
        QVERIFY(journal.contains("\"op\":\"CopyObject\""));
        QVERIFY(journal.contains("\"op\":\"DeleteObject\""));
    }

    void renamesAFolderWithEverythingBelow()
    {
        for (const char* k : {"old/", "old/a", "old/sub/b", "old/sub/deeper/c", "older.txt", "keep/x"})
            m_bucket.add(QString::fromLatin1(k), 3);
        m_bucket.sort();
        Job* job = run(QStringLiteral("old/"), true, QStringLiteral("new"));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 4);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(m_bucket.keys(), (QStringList{QStringLiteral("keep/x"), QStringLiteral("new/"), QStringLiteral("new/a"),
                                               QStringLiteral("new/sub/b"), QStringLiteral("new/sub/deeper/c"),
                                               QStringLiteral("older.txt")}));
    }

    void neverOverwrites()
    {
        m_bucket.add(QStringLiteral("a.txt"));
        m_bucket.add(QStringLiteral("b.txt"));
        m_bucket.add(QStringLiteral("d1/x"));
        m_bucket.add(QStringLiteral("d2/y"));
        m_bucket.sort();
        Job* job = run(QStringLiteral("a.txt"), false, QStringLiteral("b.txt"));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->errorText().contains(QLatin1String("exists")));
        job = run(QStringLiteral("d1/"), true, QStringLiteral("d2"));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QCOMPARE(m_bucket.copies, 0);
        QVERIFY(m_previews.isEmpty());
    }

    void aSourceChangedSinceThePlanIsLeftAlone()
    {
        m_bucket.add(QStringLiteral("d/a"));
        m_bucket.add(QStringLiteral("d/b"));
        m_bucket.sort();
        m_changeAfterPlan = {QStringLiteral("d/b")};
        Job* job = run(QStringLiteral("d/"), true, QStringLiteral("e"));
        QCOMPARE(job->outcome(), Job::Outcome::PartlyFailed);
        QCOMPARE(job->report().succeeded, 1);
        QCOMPARE(qobject_cast<RenameJob*>(job)->failedKeys(), QStringList{QStringLiteral("d/b")});
        QVERIFY(m_bucket.contains(QStringLiteral("d/b")));    // untouched
        QVERIFY(!m_bucket.contains(QStringLiteral("e/b")));   // not copied
        QVERIFY(m_bucket.contains(QStringLiteral("e/a")));
    }

    void aCopyThatDoesNotMatchKeepsTheOriginal()
    {
        m_bucket.add(QStringLiteral("a.bin"), 100);
        m_bucket.corruptCopies = {QStringLiteral("b.bin")};
        Job* job = run(QStringLiteral("a.bin"), false, QStringLiteral("b.bin"));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(m_bucket.contains(QStringLiteral("a.bin")));   // never deleted
        QVERIFY(job->report().failures.first().contains(QLatin1String("does not match")));
    }

    void objectsTooLargeForOneCopyStopThePlan()
    {
        m_bucket.add(QStringLiteral("big/huge.img"), RenameJob::kCopyLimit + 1);
        m_bucket.add(QStringLiteral("big/small"), 1);
        m_bucket.sort();
        Job* job = run(QStringLiteral("big/"), true, QStringLiteral("bigger"));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->errorText().contains(QLatin1String("5 GiB")));
        QCOMPARE(m_bucket.copies, 0);
    }

    void badNames()
    {
        m_bucket.add(QStringLiteral("a.txt"));
        QCOMPARE(run(QStringLiteral("a.txt"), false, QStringLiteral("a.txt"))->outcome(), Job::Outcome::Failed);
        QCOMPARE(run(QStringLiteral("a.txt"), false, QStringLiteral("x/y"))->outcome(), Job::Outcome::Failed);
        QCOMPARE(run(QStringLiteral("a.txt"), false, QStringLiteral(".."))->outcome(), Job::Outcome::Failed);
        QCOMPARE(m_bucket.copies, 0);
    }

private:
    Job* run(const QString& key, bool folder, const QString& name)
    {
        auto* job = new RenameJob(m_h->account, QStringLiteral("bkt"), key, folder, name, m_ops->planDir());
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        if (!done.wait(20000))
            qWarning("job did not finish");
        return job;   // deleted later, from the event loop
    }

    std::unique_ptr<AccountHarness> m_h;
    FakeBucket m_bucket;
    QStringList m_changeAfterPlan;
    QList<Job::Preview> m_previews;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
};

QTEST_GUILESS_MAIN(TestRenameJob)
#include "test_rename_job.moc"
