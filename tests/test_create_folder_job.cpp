#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CreateFolderJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/AccountHarness.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestCreateFolderJob : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_keys.clear();
        m_failPut = false;
        // A tiny bucket "bkt": HEAD / GET listing / PUT on a key set.
        m_h->server->setFallbackHandler([this](const RecordedRequest& r) {
            const QString path = QUrl::fromPercentEncoding(r.path);
            if (!path.startsWith(QLatin1String("/bkt")))
                return FakeResponse::s3Error(404, "NoSuchBucket");
            const QString key = path.mid(5);
            if (r.method == "HEAD")
                return m_keys.contains(key) ? FakeResponse().withHeader("Content-Length", "0")
                                            : FakeResponse::s3Error(404, "NotFound");
            if (r.method == "PUT") {
                if (m_failPut)
                    return FakeResponse::s3Error(403, "AccessDenied");
                m_keys.insert(key);
                return FakeResponse().withHeader("ETag", "\"d41d8cd98f00b204e9800998ecf8427e\"");
            }
            if (r.method == "GET" && r.hasQueryItem("list-type")) {
                const QString prefix = QUrl::fromPercentEncoding(r.queryItem("prefix"));
                QByteArray contents;
                for (const QString& k : std::as_const(m_keys)) {
                    if (k.startsWith(prefix))
                        contents += "<Contents><Key>" + k.toUtf8() + "</Key><Size>0</Size></Contents>";
                }
                return FakeResponse::ok("<ListBucketResult><Name>bkt</Name><IsTruncated>false</IsTruncated>"
                                        + contents + "</ListBucketResult>");
            }
            return FakeResponse::s3Error(501, "NotImplemented");
        });
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
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

    void createsAndVerifiesTheMarker()
    {
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        Job* job = start(QStringLiteral("photos/"), QStringLiteral("2026 trip"));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(job->report().verified);
        QCOMPARE(job->report().succeeded, 1);
        QVERIFY(preview.isEmpty());   // creating needs no confirmation
        QVERIFY(m_keys.contains(QStringLiteral("photos/2026 trip/")));

        RecordedRequest put;
        for (const RecordedRequest& r : m_h->server->requests()) {
            if (r.method == "PUT")
                put = r;
        }
        QCOMPARE(put.path, QByteArray("/bkt/photos/2026%20trip/"));
        QVERIFY(put.body.isEmpty());
        QCOMPARE(put.header("Content-MD5"), QCryptographicHash::hash({}, QCryptographicHash::Md5).toBase64());
        QCOMPARE(put.header("Content-Type"), QByteArray("application/x-directory"));

        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray line = f.readAll();
        QVERIFY(line.contains("\"op\":\"PutObject\""));
        QVERIFY(line.contains("photos/2026 trip/"));
        QVERIFY(!line.contains(AccountHarness::kSecret));
    }

    void anExistingFolderIsLeftAlone()
    {
        m_keys.insert(QStringLiteral("a/"));
        Job* job = start(QString(), QStringLiteral("a"));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().skipped, 1);
        QCOMPARE(putCount(), 0);

        // Implicit folder: no marker, but keys below it.
        m_keys.insert(QStringLiteral("b/x.txt"));
        job = start(QString(), QStringLiteral("b"));
        QCOMPARE(job->report().skipped, 1);
        QCOMPARE(putCount(), 0);
    }

    void badNamesSendNothing_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("empty") << QString();
        QTest::newRow("slash") << QStringLiteral("a/b");
        QTest::newRow("dot") << QStringLiteral(".");
        QTest::newRow("dotdot") << QStringLiteral("..");
        QTest::newRow("control") << QStringLiteral("a\nb");
    }

    void badNamesSendNothing()
    {
        QFETCH(QString, name);
        QVERIFY(!CreateFolderJob::validateName(name).isEmpty());
        Job* job = start(QString(), name);
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QCOMPARE(m_h->server->requestCount(), 0);
    }

    void aFailedPutIsReported()
    {
        m_failPut = true;
        Job* job = start(QString(), QStringLiteral("x"));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QCOMPARE(job->report().failed, 1);
        QCOMPARE(job->report().failures.size(), 1);
        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("\"result\":\"failed\""));
    }

    void aReadOnlyAccountCreatesNothing()
    {
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        Job* job = start(QString(), QStringLiteral("x"));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QCOMPARE(m_h->server->requestCount(), 0);
    }

private:
    // Runs a job to its end; the job stays alive until the next event loop pass.
    Job* start(const QString& parent, const QString& name)
    {
        auto* job = new CreateFolderJob(*m_h->accounts->account(m_h->account.id), QStringLiteral("bkt"), parent, name);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        if (!done.wait(10000))
            qWarning("job did not finish");
        return job;   // deleted later, from the event loop

    }
    int putCount() const
    {
        int n = 0;
        for (const RecordedRequest& r : m_h->server->requests())
            n += r.method == "PUT" ? 1 : 0;
        return n;
    }

    std::unique_ptr<AccountHarness> m_h;
    QSet<QString> m_keys;
    bool m_failPut = false;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
};

QTEST_GUILESS_MAIN(TestCreateFolderJob)
#include "test_create_folder_job.moc"
