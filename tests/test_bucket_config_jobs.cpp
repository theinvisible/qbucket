// SetBucketConfigJob (a configuration written, read back, removed; what the
// preview warns about; unconfirmed writes refused; a proxy that runs
// DeleteBucket instead) and the governance bypass of DeleteVersionsJob
// (offered on object lock, never preset, journaled; COMPLIANCE and legal
// holds still hold).

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/DeleteVersionsJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/SetBucketConfigJob.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/BucketConfig.h>
#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QFile>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");

QByteArray corsDocument(const QString& origin = QStringLiteral("https://example.com"))
{
    s3::CorsConfiguration c;
    s3::CorsRule r;
    r.allowedMethods = {QStringLiteral("GET")};
    r.allowedOrigins = {origin};
    c.rules = {r};
    return s3::buildCorsConfiguration(c).value();
}

} // namespace

class TestBucketConfigJobs : public QObject {
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
    bool m_bypass = false;   // what the "user" ticks in the preview

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    void probe()
    {
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY(m_caps->isLoaded(m_h->account.id));
        m_caps->bucketOpened(m_h->account.id, kBucket);
        QTRY_VERIFY_WITH_TIMEOUT(!m_caps->isProbing(m_h->account.id, kBucket) && m_caps->bucketSet(m_h->account.id, kBucket),
                                 10000);
    }

    // Runs a job to its end; the outcome (and the error text, read before
    // the job is gone).
    std::pair<Job::Outcome, QString> run(Job* job, Job::Report* report = nullptr)
    {
        if (auto* config = qobject_cast<SetBucketConfigJob*>(job))
            config->setReadBackDelayForTesting(10);
        if (auto* versions = qobject_cast<DeleteVersionsJob*>(job))
            versions->setRecheckDelayForTesting(10);
        std::optional<std::pair<Job::Outcome, QString>> out;
        connect(job, &Job::finished, this, [&out, job, report] {
            out = {job->outcome(), job->errorText() + QLatin1Char('|') + job->report().verification};
            if (report)
                *report = job->report();
        });
        m_ops->start(job);
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 20000))
            return {Job::Outcome::None, QStringLiteral("timeout")};
        return *out;
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
        m_objects->putObject(kBucket, QStringLiteral("keep.txt"), "x");
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
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            job->setBypassGovernance(m_bypass);
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

    void writeReadBackAndRemove()
    {
        probe();
        const QByteArray doc = corsDocument();
        auto [outcome, text] = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Cors, doc));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QVERIFY2(text.contains(QLatin1String("as sent")), qPrintable(text));
        QCOMPARE(m_previews.size(), 1);
        QVERIFY(m_previews.first().document.contains(QLatin1String("<AllowedOrigin>https://example.com")));
        QVERIFY(m_previews.first().currentDocument.isEmpty());
        QCOMPARE(m_previews.first().objects, qint64(-1));
        QVERIFY(m_objects->bucketConfig(kBucket, s3::BucketConfig::Cors).has_value());

        // The same again: nothing to confirm, nothing sent.
        const int puts = m_objects->count(QStringLiteral("PutBucketCors"));
        std::tie(outcome, text) = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Cors, doc));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QVERIFY(text.contains(QLatin1String("unchanged")));
        QCOMPARE(m_previews.size(), 1);
        QCOMPARE(m_objects->count(QStringLiteral("PutBucketCors")), puts);

        // A change shows what is set now.
        std::tie(outcome, text) = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Cors,
                                                              corsDocument(QStringLiteral("https://other.example"))));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QVERIFY(m_previews.last().currentDocument.contains(QLatin1String("https://example.com")));

        std::tie(outcome, text) = run(SetBucketConfigJob::remove(m_h->account, kBucket, s3::BucketConfig::Cors));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QVERIFY(text.contains(QLatin1String("nothing is set")));
        QVERIFY(!m_objects->bucketConfig(kBucket, s3::BucketConfig::Cors).has_value());
        const QByteArray journal = journalText();
        QVERIFY(journal.contains("PutBucketCors"));
        QVERIFY(journal.contains("DeleteBucketCors"));
    }

    void cannedBucketAcl()
    {
        probe();
        auto [outcome, text] = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Acl, {},
                                                           QStringLiteral("public-read")));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QCOMPARE(*m_objects->bucketConfig(kBucket, s3::BucketConfig::Acl), QByteArray("public-read"));
        QVERIFY(!m_previews.last().warnings.isEmpty());
        QVERIFY(m_previews.last().typeToConfirm.isEmpty());
        std::tie(outcome, text) = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Acl, {},
                                                              QStringLiteral("public-read-write")));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QCOMPARE(m_previews.last().typeToConfirm, kBucket);   // anyone may write: typed
    }

    void thePreviewSaysWhatItDoes()
    {
        probe();
        // A policy letting anyone delete: warned and typed; a Deny: warned.
        const QByteArray policy = "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Principal\":\"*\","
                                  "\"Action\":\"s3:*\",\"Resource\":\"arn:aws:s3:::bkt/*\"},{\"Effect\":\"Deny\","
                                  "\"Principal\":\"*\",\"Action\":\"s3:DeleteBucket\",\"Resource\":\"arn:aws:s3:::bkt\"}]}";
        QCOMPARE(run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Policy, policy)).first,
                 Job::Outcome::Succeeded);
        const Job::Preview p = m_previews.last();
        QCOMPARE(p.typeToConfirm, kBucket);
        QCOMPARE(p.warnings.size(), 2);

        // Lifecycle rules that delete say so.
        s3::LifecycleConfiguration lc;
        s3::LifecycleRule rule;
        rule.id = QStringLiteral("tmp");
        rule.prefix = QStringLiteral("tmp/");
        rule.expirationDays = 7;
        rule.noncurrentDays = 3;
        lc.rules = {rule};
        QCOMPARE(run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Lifecycle,
                                             s3::buildLifecycleConfiguration(lc).value())).first,
                 Job::Outcome::Succeeded);
        const QString warnings = m_previews.last().warnings.join(u'\n');
        QVERIFY2(warnings.contains(QStringLiteral("“tmp/”")), qPrintable(warnings));
        QVERIFY(warnings.contains(QLatin1String("earlier versions")));
    }

    void invalidDocumentsAreNeverSent()
    {
        probe();
        s3::LifecycleConfiguration lc;
        lc.rules = {s3::LifecycleRule{}};   // no action
        auto [outcome, text] = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Lifecycle,
                                                           s3::buildLifecycleConfiguration(lc).value()));
        QCOMPARE(outcome, Job::Outcome::Failed);
        QVERIFY(text.contains(QLatin1String("not valid")));
        QCOMPARE(m_objects->count(QStringLiteral("PutBucketLifecycleConfiguration")), 0);
        std::tie(outcome, text) = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Website,
                                                              "<WebsiteConfiguration/>"));
        QCOMPARE(outcome, Job::Outcome::Failed);
    }

    void unconfirmedWritesAreRefused()
    {
        useProvider(QStringLiteral("generic"));
        probe();
        auto [outcome, text] = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Cors, corsDocument()));
        QCOMPARE(outcome, Job::Outcome::Refused);
        QVERIFY2(text.contains(QLatin1String("not confirmed")), qPrintable(text));
        QCOMPARE(m_objects->count(QStringLiteral("PutBucketCors")), 0);
    }

    // A proxy that drops ?cors: PUT ran as CreateBucket (409), DELETE as
    // DeleteBucket — refused while the bucket holds an object, the bucket
    // gone when it doesn't. Both end in a failure that says so.
    void aProxyThatDropsTheSubresource()
    {
        probe();
        m_objects->setInterceptSubresourceWrites(true);
        auto [outcome, text] = run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Cors, corsDocument()));
        QCOMPARE(outcome, Job::Outcome::Failed);
        QVERIFY2(text.contains(QLatin1String("still there")), qPrintable(text));

        m_objects->setInterceptSubresourceWrites(false);
        QCOMPARE(run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::Cors, corsDocument())).first,
                 Job::Outcome::Succeeded);
        m_objects->setInterceptSubresourceWrites(true);
        std::tie(outcome, text) = run(SetBucketConfigJob::remove(m_h->account, kBucket, s3::BucketConfig::Cors));
        QCOMPARE(outcome, Job::Outcome::Failed);   // 409 BucketNotEmpty: the guard object
        QVERIFY(m_objects->hasBucket(kBucket));

        m_objects->deleteObject(kBucket, QStringLiteral("keep.txt"));   // now empty: the next DELETE takes the bucket
        std::tie(outcome, text) = run(SetBucketConfigJob::remove(m_h->account, kBucket, s3::BucketConfig::Cors));
        QCOMPARE(outcome, Job::Outcome::Failed);
        QVERIFY2(text.contains(QLatin1String("is gone")), qPrintable(text));
        QVERIFY(!m_objects->hasBucket(kBucket));
    }

    void complianceIsTyped()
    {
        m_objects->setObjectLockEnabled(kBucket);
        probe();
        s3::ObjectLockConfiguration c;
        c.enabled = true;
        c.mode = s3::ObjectLockMode::Compliance;
        c.days = 1;
        QCOMPARE(run(SetBucketConfigJob::put(m_h->account, kBucket, s3::BucketConfig::ObjectLock,
                                             s3::buildObjectLockConfiguration(c))).first,
                 Job::Outcome::Succeeded);
        QCOMPARE(m_previews.last().typeToConfirm, kBucket);
        QVERIFY(m_previews.last().warnings.join(u' ').contains(QLatin1String("COMPLIANCE")));
    }

    // Deleting versions under GOVERNANCE retention: refused without the
    // bypass, done with it (offered on object lock only, journaled); a legal
    // hold holds against it.
    void theGovernanceBypass()
    {
        m_objects->setObjectLockEnabled(kBucket);
        const QString kept = m_objects->putObject(kBucket, QStringLiteral("l/kept.txt"), "x");
        const QString held = m_objects->putObject(kBucket, QStringLiteral("l/held.txt"), "y");
        m_objects->setRetention(kBucket, QStringLiteral("l/kept.txt"), kept, s3::ObjectLockMode::Governance,
                                QDateTime::currentDateTimeUtc().addDays(1));
        m_objects->setLegalHold(kBucket, QStringLiteral("l/held.txt"), held, true);
        probe();

        Job::Report report;
        auto [outcome, text] = run(DeleteVersionsJob::everything(m_h->account, kBucket, {{QStringLiteral("l/"), true}},
                                                                 m_ops->planDir()),
                                   &report);
        QCOMPARE(outcome, Job::Outcome::Failed);   // nothing could go
        QVERIFY(m_previews.last().bypassOffer);
        QCOMPARE(report.failed, 2);
        QVERIFY(report.failures.join(u' ').contains(QLatin1String("object lock")));

        m_bypass = true;
        std::tie(outcome, text) = run(DeleteVersionsJob::everything(m_h->account, kBucket, {{QStringLiteral("l/"), true}},
                                                                    m_ops->planDir()),
                                      &report);
        QCOMPARE(outcome, Job::Outcome::PartlyFailed);
        QCOMPARE(report.succeeded, 1);   // the retained one
        QCOMPARE(report.failed, 1);      // the held one
        QCOMPARE(m_objects->versionedKeys(kBucket).filter(QStringLiteral("l/")), QStringList{QStringLiteral("l/held.txt")});
        QVERIFY(journalText().contains("\"bypassGovernance\":true"));
    }

    void noBypassWithoutObjectLock()
    {
        m_objects->setVersioning(kBucket, QStringLiteral("Enabled"));
        m_objects->putObject(kBucket, QStringLiteral("v/a.txt"), "x");
        probe();
        m_bypass = true;
        QCOMPARE(run(DeleteVersionsJob::everything(m_h->account, kBucket, {{QStringLiteral("v/"), true}},
                                                   m_ops->planDir())).first,
                 Job::Outcome::Succeeded);
        QVERIFY(!m_previews.last().bypassOffer);
        QVERIFY(!journalText().contains("bypassGovernance"));
    }
};

QTEST_GUILESS_MAIN(TestBucketConfigJobs)
#include "test_bucket_config_jobs.moc"
