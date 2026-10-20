// The app's mutation jobs against a real S3 server — opt-in, like
// libs/qbucket-s3/tests/it_s3_backend: runs when QBUCKET_IT_ENDPOINT is set
// (QBUCKET_IT_REGION, _ACCESS_KEY, _SECRET_KEY, _BUCKET, _ADDRESSING,
// _PROVIDER, _BACKEND; optional _CREATE_BUCKET=1 for local backends).
//
// Safety: refuses any bucket not named qbucket-it-*; every key lives under
// it/<run id>/ and only that is cleaned up. The account is "Development",
// so no production confirmations are involved — the gate still runs.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CreateFolderJob.h"
#include "controllers/DeleteJob.h"
#include "controllers/RenameJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/FakeKeychainBackend.h"

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"

#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

QString env(const char* name)
{
    return qEnvironmentVariable(name);
}

} // namespace

class ItOperations : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (env("QBUCKET_IT_ENDPOINT").isEmpty())
            QSKIP("QBUCKET_IT_ENDPOINT not set — integration tests are opt-in");
        m_bucket = env("QBUCKET_IT_BUCKET");
        static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
        QVERIFY2(safeName.match(m_bucket).hasMatch(),
                 "refusing to run: QBUCKET_IT_BUCKET must be a dedicated qbucket-it-* bucket");
        QVERIFY(m_dir.isValid());
        m_root = QStringLiteral("it/%1/").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts.ini")));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("it-%1").arg(env("QBUCKET_IT_BACKEND"));
        a.providerId = env("QBUCKET_IT_PROVIDER").isEmpty() ? QStringLiteral("generic") : env("QBUCKET_IT_PROVIDER");
        a.region = env("QBUCKET_IT_REGION");
        a.endpoint = env("QBUCKET_IT_ENDPOINT");
        a.addressing = env("QBUCKET_IT_ADDRESSING") == QLatin1String("virtual") ? s3::AddressingStyle::VirtualHosted
                                                                                : s3::AddressingStyle::Path;
        a.accessKeyId = env("QBUCKET_IT_ACCESS_KEY");
        a.environment = Environment::Development;
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                env("QBUCKET_IT_SECRET_KEY"));
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        QVERIFY(m_accounts->save(a));
        m_account = a;
        m_sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
        m_store = std::make_unique<CapabilityStore>(m_dir.filePath(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_accounts.get(), m_sessions.get(), m_store.get(), m_log.get());
        m_journal = std::make_unique<Journal>(m_dir.filePath(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_accounts.get(), m_sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());

        if (env("QBUCKET_IT_CREATE_BUCKET") == QLatin1String("1")) {
            s3::CreateBucketParams params;
            params.bucket = m_bucket;
            params.region = a.region;
            QObject ctx;
            bool done = false;
            session()->createBucket(params, &ctx, [&](const s3::Result<void>&) { done = true; });   // exists: fine
            QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        }
    }

    void cleanupTestCase()
    {
        if (!m_sessions)
            return;
        // Everything below this run's root, and nothing else.
        QStringList keys;
        QString token;
        for (int page = 0; page < 100; ++page) {
            s3::ListObjectsParams p;
            p.bucket = m_bucket;
            p.prefix = m_root;
            p.continuationToken = token;
            std::optional<s3::Result<s3::ListPage>> r;
            QObject ctx;
            session()->listObjects(p, &ctx, [&](s3::Result<s3::ListPage> res) { r.emplace(std::move(res)); });
            QTRY_VERIFY_WITH_TIMEOUT(r.has_value(), 30000);
            if (!*r)
                break;
            for (const s3::ObjectEntry& e : (*r)->objects)
                keys.append(e.key);
            if (!(*r)->isTruncated)
                break;
            token = (*r)->nextContinuationToken;
        }
        for (const QString& k : std::as_const(keys)) {
            QVERIFY(k.startsWith(m_root));
            QObject ctx;
            bool done = false;
            session()->deleteObject(m_bucket, k, QString(), &ctx, [&](const s3::Result<s3::DeleteObjectResult>&) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        }
        m_ops.reset();
        m_journal.reset();
        m_caps.reset();
        m_sessions.reset();
        m_secrets.reset();
    }

    void createFolder()
    {
        Job* job = run(new CreateFolderJob(m_account, m_bucket, m_root, QStringLiteral("new folder ä")));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(job->report().succeeded, 1);

        // Again: it exists now, nothing is written.
        job = run(new CreateFolderJob(m_account, m_bucket, m_root, QStringLiteral("new folder ä")));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().skipped, 1);
    }

    void deleteAFolderRecursively()
    {
        const QString folder = m_root + QStringLiteral("doomed/");
        const QStringList keys = {folder + QStringLiteral("a.txt"), folder + QStringLiteral("sub/b c.txt"),
                                  folder + QStringLiteral("sub/deeper/ü+%.bin"), folder};
        for (const QString& key : keys)
            QVERIFY(put(key, key.endsWith(u'/') ? QByteArray() : key.toUtf8()));
        const QString keep = m_root + QStringLiteral("doomed-not.txt");   // shares the prefix text
        QVERIFY(put(keep, "keep"));

        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));
        auto* job = new DeleteJob(m_account, m_bucket, {{folder, true}}, m_ops->planDir());
        job->setRecheckDelaysForTesting(2000, 5000);
        run(job);
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, keys.size());
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QVERIFY(exists(keep));
        for (const QString& key : keys)
            QVERIFY2(!exists(key), qPrintable(key));
    }

    void renameAnObjectAndAFolder()
    {
        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));
        const QString file = m_root + QStringLiteral("before ä.txt");
        QVERIFY(put(file, "rename me"));
        Job* job = run(new RenameJob(m_account, m_bucket, file, false, QStringLiteral("after ü.txt"), m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QVERIFY(!exists(file));
        QVERIFY(exists(m_root + QStringLiteral("after ü.txt")));

        const QString folder = m_root + QStringLiteral("dir old/");
        const QStringList keys = {folder, folder + QStringLiteral("a"), folder + QStringLiteral("sub/b+c")};
        for (const QString& k : keys)
            QVERIFY(put(k, k.endsWith(u'/') ? QByteArray() : k.toUtf8()));
        job = run(new RenameJob(m_account, m_bucket, folder, true, QStringLiteral("dir new"), m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, keys.size());
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        for (const QString& k : keys) {
            QVERIFY2(!exists(k), qPrintable(k));
            QVERIFY2(exists(m_root + QStringLiteral("dir new/") + k.mid(folder.size())), qPrintable(k));
        }
    }

private:
    bool put(const QString& key, const QByteArray& data)
    {
        s3::PutObjectParams p;
        p.bucket = m_bucket;
        p.key = key;
        p.data = data;
        std::optional<bool> ok;
        QObject ctx;
        session()->putObject(p, &ctx, [&](const s3::Result<s3::PutObjectResult>& r) { ok = bool(r); });
        return QTest::qWaitFor([&] { return ok.has_value(); }, 30000) && *ok;
    }

    bool exists(const QString& key)
    {
        std::optional<bool> found;
        QObject ctx;
        session()->headObject(m_bucket, key, QString(), &ctx, [&](const s3::Result<s3::ObjectMeta>& r) { found = bool(r); });
        return QTest::qWaitFor([&] { return found.has_value(); }, 30000) && *found;
    }

    s3::Session* session()
    {
        const s3::Result<s3::Session*> s = m_sessions->session(m_account.id);
        return s ? *s : nullptr;
    }

    // Starts a job, confirms its preview, waits for the end.
    Job* run(Job* job)
    {
        QObject::connect(m_ops.get(), &OperationController::previewNeeded, job, [job](Job* j) {
            if (j == job)
                j->confirm();
        });
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        if (!done.wait(120000))
            qWarning("job did not finish");
        if (job->outcome() != Job::Outcome::Succeeded)
            qWarning().noquote() << OperationController::summary(*job) << job->report().failures;
        return job;
    }

    QTemporaryDir m_dir;
    QString m_bucket;
    QString m_root;
    Account m_account;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
};

QTEST_GUILESS_MAIN(ItOperations)
#include "it_operations.moc"
