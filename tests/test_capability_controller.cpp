#include "accounts/AccountStore.h"
#include "accounts/CapabilityStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "support/FakeKeychainBackend.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using F = qbucket::s3::Feature;
using S = qbucket::s3::CapabilityState;

namespace {

const QString kKeyId = QStringLiteral("AKIDCAPS");
const QString kSecret = QStringLiteral("caps-secret");
const QByteArray kBuckets = "<ListAllMyBucketsResult><Buckets>"
                            "<Bucket><Name>bkt</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                            "</Buckets></ListAllMyBucketsResult>";

} // namespace

class TestCapabilityController : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_server = std::make_unique<FakeS3Server>();
        QVERIFY(m_server->listen());
        s3::Credentials c;
        c.accessKeyId = kKeyId;
        c.secretAccessKey = kSecret;
        m_server->setVerifySignatures(c, "test-1");
        m_lifecycle = FakeResponse::ok("<LifecycleConfiguration><Rule><ID>r</ID></Rule></LifecycleConfiguration>");
        m_deleteAnswer = FakeResponse::ok("<DeleteResult/>");
        m_failEverything = false;
        m_server->setFallbackHandler([this](const RecordedRequest& r) { return answer(r); });

        ++m_run;
        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(m_run)));
        auto backend = std::make_unique<qbucket::testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("lab");
        a.providerId = QStringLiteral("generic");
        a.region = QStringLiteral("test-1");
        a.endpoint = m_server->endpointUrl().toString();
        a.accessKeyId = kKeyId;
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                kSecret);
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        QVERIFY(m_accounts->save(a));
        m_account = a.id;
        m_sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
        m_store = std::make_unique<CapabilityStore>(m_dir.filePath(QStringLiteral("caps-%1").arg(m_run)));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_accounts.get(), m_sessions.get(), m_store.get(),
                                                        m_log.get());
    }

    void cleanup()
    {
        m_caps.reset();
        m_sessions.reset();
        m_store.reset();
        m_log.reset();
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void connectingLoadsTheStoreAndProbesTheServer()
    {
        // A result from yesterday is on disk.
        s3::CapabilitySet old;
        old.probedAt = QDateTime::currentDateTimeUtc().addDays(-2);
        m_store->save(m_account, old);
        m_store->waitForIdle();

        QSignalSpy updated(m_caps.get(), &CapabilityController::capabilitiesUpdated);
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->serverSet(m_account).probedAt > old.probedAt, 10000);   // expired: probed
        QCOMPARE(m_server->requests().first().path, QByteArray("/"));
        QVERIFY(!updated.isEmpty());
    }

    void aBucketIsProbedWhenOpenedAndThenKeptFresh()
    {
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        m_caps->bucketOpened(m_account, QStringLiteral("bkt"));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_account, QStringLiteral("bkt")).has_value(), 10000);
        QVERIFY(!m_caps->isProbing(m_account, QStringLiteral("bkt")));
        QCOMPARE(m_caps->entry(m_account, QStringLiteral("bkt"), F::Lifecycle).state, S::Supported);
        QCOMPARE(m_caps->entry(m_account, QStringLiteral("bkt"), F::Website).state, S::Unsupported);   // 501
        QCOMPARE(m_caps->bucketSet(m_account, QStringLiteral("bkt"))->bucketState.versioning, QStringLiteral("Enabled"));

        const int requests = m_server->requestCount();
        m_caps->bucketOpened(m_account, QStringLiteral("bkt"));   // fresh: nothing sent
        QTest::qWait(200);
        QCOMPARE(m_server->requestCount(), requests);

        // Saved for the next run.
        m_store->waitForIdle();
        QVERIFY(QFile::exists(m_store->rootDir() + u'/' + m_account.toString(QUuid::WithoutBraces)
                              + QStringLiteral("/bucket-bkt.json")));
    }

    void downgradesAtOnceAndAFailedProbeFlipsNothing()
    {
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        m_caps->bucketOpened(m_account, QStringLiteral("bkt"));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_account, QStringLiteral("bkt")).has_value(), 10000);
        QSignalSpy changes(m_caps.get(), &CapabilityController::changesDetected);

        // A server error is no answer: Lifecycle stays Supported.
        m_lifecycle = FakeResponse::s3Error(503, "SlowDown");
        recheckAndWait();
        QCOMPARE(m_caps->entry(m_account, QStringLiteral("bkt"), F::Lifecycle).state, S::Supported);
        QVERIFY(changes.isEmpty());

        // A definitive "no": downgraded at once, reported, logged, kept.
        m_lifecycle = FakeResponse::s3Error(501, "NotImplemented");
        recheckAndWait();
        QCOMPARE(m_caps->entry(m_account, QStringLiteral("bkt"), F::Lifecycle).state, S::Unsupported);
        QCOMPARE(changes.size(), 1);
        const auto list = changes.first().at(2).value<QList<s3::CapabilityChange>>();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().feature, F::Lifecycle);
        QCOMPARE(list.first().before, S::Supported);
        QVERIFY(m_log->entries().last().text.contains(CapabilityController::featureName(F::Lifecycle)));

        QObject context;
        std::optional<QList<QJsonObject>> history;
        m_store->loadHistory(m_account, &context, [&](const QList<QJsonObject>& h) { history = h; });
        QTRY_VERIFY(history.has_value());
        QCOMPARE(history->size(), 1);
        QCOMPARE(history->first().value(QStringLiteral("feature")).toString(), QStringLiteral("bucket.lifecycle"));
    }

    void decisions()
    {
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        const QString b = QStringLiteral("bkt");
        // Nothing known yet: unverified for reading, blocked for destroying.
        QCOMPARE(m_caps->decide(m_account, b, F::Lifecycle, false), s3::Decision::AllowedUnverified);
        QCOMPARE(m_caps->decide(m_account, b, F::Lifecycle, true), s3::Decision::Blocked);
        QVERIFY(!CapabilityController::reason(F::Lifecycle, m_caps->entry(m_account, b, F::Lifecycle), true).isEmpty());

        m_caps->bucketOpened(m_account, b);
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_account, b).has_value(), 10000);
        QCOMPARE(m_caps->decide(m_account, b, F::Lifecycle, true), s3::Decision::Allowed);
        QCOMPARE(m_caps->decide(m_account, b, F::Website, false), s3::Decision::Blocked);
        QVERIFY(CapabilityController::reason(F::Website, m_caps->entry(m_account, b, F::Website), false)
                    .contains(QLatin1String("501")));
        QVERIFY(CapabilityController::reason(F::Lifecycle, m_caps->entry(m_account, b, F::Lifecycle), true).isEmpty());
    }

    void ensureFreshProbesOnlyWhenTooOld()
    {
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        QObject context;
        std::optional<bool> fresh;
        m_caps->ensureFresh(m_account, QStringLiteral("bkt"), 600, &context, [&](bool ok) { fresh = ok; });
        QVERIFY(!fresh.has_value());   // never inside the call
        QTRY_VERIFY_WITH_TIMEOUT(fresh.has_value(), 10000);
        QVERIFY(*fresh);

        const int requests = m_server->requestCount();
        fresh.reset();
        m_caps->ensureFresh(m_account, QStringLiteral("bkt"), 600, &context, [&](bool ok) { fresh = ok; });
        QTRY_VERIFY(fresh.has_value());
        QCOMPARE(m_server->requestCount(), requests);   // young enough

        fresh.reset();
        m_caps->ensureFresh(m_account, QStringLiteral("bkt"), 0, &context, [&](bool ok) { fresh = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(fresh.has_value(), 10000);
        QVERIFY(m_server->requestCount() > requests);    // probed again

        // A probe that fails as a whole is no green light.
        m_failEverything = true;
        fresh.reset();
        m_caps->ensureFresh(m_account, QStringLiteral("bkt"), 0, &context, [&](bool ok) { fresh = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(fresh.has_value(), 10000);
        QVERIFY(!*fresh);
    }

    void aRefusedRequestIsLearned()
    {
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        QSignalSpy changes(m_caps.get(), &CapabilityController::changesDetected);
        m_deleteAnswer = FakeResponse::s3Error(501, "NotImplemented");
        QObject context;
        bool done = false;
        (*m_sessions->session(m_account))
            ->deleteObjects(QStringLiteral("bkt"), {s3::ObjectIdentifier{QStringLiteral("x"), {}}}, &context,
                            [&](const s3::Result<s3::DeleteObjectsResult>&) { done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QTRY_COMPARE(m_caps->entry(m_account, QStringLiteral("bkt"), F::BatchDelete).state, S::Unsupported);
        QCOMPARE(m_caps->entry(m_account, QStringLiteral("bkt"), F::BatchDelete).source, s3::CapabilitySource::Learned);
        QCOMPARE(changes.size(), 1);
    }

    void aRemovedAccountTakesItsResultsAlong()
    {
        QVERIFY(m_sessions->session(m_account));
        QTRY_VERIFY(m_caps->isLoaded(m_account));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->serverSet(m_account).probedAt.isValid(), 10000);
        m_store->waitForIdle();
        const QString dir = m_store->rootDir() + u'/' + m_account.toString(QUuid::WithoutBraces);
        QVERIFY(QDir(dir).exists());
        QVERIFY(m_accounts->remove(m_account));
        m_store->waitForIdle();
        QVERIFY(!QDir(dir).exists());
        QVERIFY(!m_caps->isLoaded(m_account));
    }

private:
    void recheckAndWait()
    {
        QSignalSpy updated(m_caps.get(), &CapabilityController::capabilitiesUpdated);
        m_caps->recheck(m_account, QStringLiteral("bkt"));
        QVERIFY(m_caps->isProbing(m_account, QStringLiteral("bkt")));
        QTRY_VERIFY_WITH_TIMEOUT(!m_caps->isProbing(m_account, QStringLiteral("bkt")), 20000);
    }

    FakeResponse answer(const RecordedRequest& r)
    {
        if (m_failEverything)
            return FakeResponse::s3Error(403, "InvalidAccessKeyId");
        if (r.path == "/")
            return FakeResponse::ok(kBuckets);
        if (r.method == "POST" && r.hasQueryItem("delete"))
            return m_deleteAnswer;
        if (r.method == "GET" && r.hasQueryItem("lifecycle"))
            return m_lifecycle;
        if (r.method == "GET" && r.hasQueryItem("versioning"))
            return FakeResponse::ok("<VersioningConfiguration><Status>Enabled</Status></VersioningConfiguration>");
        if (r.method == "GET" && r.hasQueryItem("list-type"))
            return FakeResponse::ok("<ListBucketResult><Name>bkt</Name><IsTruncated>false</IsTruncated></ListBucketResult>");
        if (r.method == "GET" && !r.query.isEmpty())
            return FakeResponse::s3Error(501, "NotImplemented");
        return FakeResponse();
    }

    QTemporaryDir m_dir;
    int m_run = 0;
    FakeResponse m_lifecycle;
    FakeResponse m_deleteAnswer = FakeResponse::ok("<DeleteResult/>");
    bool m_failEverything = false;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    QUuid m_account;
};

QTEST_GUILESS_MAIN(TestCapabilityController)
#include "test_capability_controller.moc"
