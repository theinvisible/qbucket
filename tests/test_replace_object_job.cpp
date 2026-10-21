// ReplaceObjectJob through the OperationController against FakeObjectStore:
// the content of an object replaced from a local file, everything else kept
// (headers, metadata, class, tags, ACL); a change on the server since the
// file was opened is a conflict (skip, overwrite, keep both); If-Match where
// confirmed, HEAD right before otherwise; what can't be carried is refused.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/ReplaceObjectJob.h"
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
const QString kKey = QStringLiteral("docs/report.txt");
} // namespace

class TestReplaceObjectJob : public QObject {
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
    QString m_file;
    QString m_openedEtag;

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    void writeLocal(const QByteArray& content)
    {
        QFile f(m_file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content);
    }

    ReplaceObjectJob* make()
    {
        auto* job = new ReplaceObjectJob(m_h->account, kBucket, {kKey, m_file, m_openedEtag});
        job->setRecheckDelay(10);
        return job;
    }

    bool run(Job* job)
    {
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        return done.wait(30000) || done.size() == 1;
    }

    static QString reasons(const Job* job)
    {
        return (job->report().notes + job->report().failures).join(QStringLiteral(" | ")) + job->errorText();
    }

    int putsWith(const QByteArray& header) const
    {
        int n = 0;
        for (const RecordedRequest& r : m_h->server->requests()) {
            if (r.method == "PUT" && r.path.endsWith("report.txt") && !r.header(header).isNull())
                ++n;
        }
        return n;
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        useProvider(QStringLiteral("aws"));   // tag / ACL writes and If-Match confirmed there
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
        m_previews.clear();
        m_beforeConfirm = nullptr;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            if (m_beforeConfirm)
                m_beforeConfirm(job);
            job->confirm();
        });
        m_objects->putObject(kBucket, kKey, "the old content", QStringLiteral("text/plain; charset=utf-8"));
        m_openedEtag = m_objects->object(kBucket, kKey)->etag;
        m_file = m_h->path(QStringLiteral("report.txt"));
        writeLocal("the new content, edited");
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

    void replacesTheContentAndKeepsTheRest()
    {
        m_objects->setObjectHeader(kBucket, kKey, QStringLiteral("cache-control"), QStringLiteral("max-age=60"));
        m_objects->setObjectHeader(kBucket, kKey, QStringLiteral("content-disposition"), QStringLiteral("inline"));
        m_objects->setObjectTags(kBucket, kKey, {{QStringLiteral("team"), QStringLiteral("ops")}});
        m_objects->setObjectAcl(kBucket, kKey, QStringLiteral("public-read"));
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification + reasons(job)));
        const auto o = m_objects->object(kBucket, kKey);
        QCOMPARE(o->data, QByteArray("the new content, edited"));
        QCOMPARE(o->contentType, QStringLiteral("text/plain; charset=utf-8"));
        QCOMPARE(o->headers.value(QStringLiteral("cache-control")), QStringLiteral("max-age=60"));
        QCOMPARE(o->headers.value(QStringLiteral("content-disposition")), QStringLiteral("inline"));
        QCOMPARE(o->tags, (s3::TagSet{{QStringLiteral("team"), QStringLiteral("ops")}}));
        QCOMPARE(o->acl, QStringLiteral("public-read"));
        // AWS confirms If-Match on PUT: the write itself was conditional.
        QCOMPARE(putsWith("if-match"), 1);
        // Unversioned bucket: the preview said the old content is gone for good.
        QCOMPARE(m_previews.size(), 1);
        QVERIFY(m_previews.first().warnings.join(u' ').contains(QLatin1String("for good")));
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("public-read")));
        QCOMPARE(job->writtenKey(), kKey);
        QCOMPARE(job->writtenEtag(), o->etag);
    }

    void changedOnTheServerIsAConflict()
    {
        m_objects->putObject(kBucket, kKey, "somebody else's content", QStringLiteral("text/plain"));
        // Skip (the default): nothing uploaded.
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(m_previews.last().conflicts, 1);
        QCOMPARE(job->report().skipped, 1);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("somebody else's content"));

        // Keep both: the edit goes next to it.
        m_beforeConfirm = [](Job* j) { j->setConflictPolicy(Job::ConflictPolicy::KeepBoth); };
        job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        const QString copy = QStringLiteral("docs/report (1).txt");
        QCOMPARE(job->writtenKey(), copy);
        QCOMPARE(m_objects->object(kBucket, copy)->data, QByteArray("the new content, edited"));
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("somebody else's content"));

        // Overwrite, as chosen.
        m_beforeConfirm = [](Job* j) { j->setConflictPolicy(Job::ConflictPolicy::Overwrite); };
        job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("the new content, edited"));
    }

    void withoutConditionalOverwriteAHeadGuardsTheWrite()
    {
        useProvider(QStringLiteral("generic"));
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(putsWith("if-match"), 0);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("the new content, edited"));

        // Changed between the preview and the upload: left alone.
        m_openedEtag = m_objects->object(kBucket, kKey)->etag;
        writeLocal("third");
        m_beforeConfirm = [this](Job*) { m_objects->putObject(kBucket, kKey, "raced in", QStringLiteral("text/plain")); };
        job = make();
        QVERIFY(run(job));
        QCOMPARE(job->report().failed, 1);
        QVERIFY(reasons(job).contains(QLatin1String("meanwhile")));
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("raced in"));
    }

    void ifMatchCatchesARaceToo()
    {
        m_beforeConfirm = [this](Job*) { m_objects->putObject(kBucket, kKey, "raced in", QStringLiteral("text/plain")); };
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(job->report().failed, 1);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("raced in"));
    }

    void whatCantBeCarriedIsRefused()
    {
        m_objects->setObjectHeader(kBucket, kKey, QStringLiteral("x-amz-server-side-encryption-customer-algorithm"),
                                   QStringLiteral("AES256"));
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->errorText().contains(QLatin1String("SSE-C")));
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("the old content"));
    }

    void deletedMeanwhileIsRecreatedOnlyWhenChosen()
    {
        m_objects->deleteObject(kBucket, kKey);
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(job->report().skipped, 1);
        QVERIFY(!m_objects->object(kBucket, kKey));
        m_beforeConfirm = [](Job* j) { j->setConflictPolicy(Job::ConflictPolicy::Overwrite); };
        job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("the new content, edited"));
        QCOMPARE(m_objects->object(kBucket, kKey)->contentType, QStringLiteral("text/plain"));   // from the name
    }

    void versionedNonProductionNeedsNoPreview()
    {
        m_objects->setVersioning(kBucket, QStringLiteral("Enabled"));
        Account a = m_h->account;
        a.environment = Environment::Development;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        auto* job = make();
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(m_previews.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestReplaceObjectJob)
#include "test_replace_object_job.moc"
