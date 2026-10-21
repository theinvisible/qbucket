#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Job.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/AccountHarness.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/ThemeManager.h"

#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QByteArray kBuckets = "<ListAllMyBucketsResult><Buckets>"
                            "<Bucket><Name>bkt</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                            "</Buckets></ListAllMyBucketsResult>";

// Jobs whose execute() ran — still known after they were deleted.
QSet<const void*> executedEver;

// Plans `objects` keys, "changes" them by journaling, verifies.
class TestJob : public Job {
public:
    TestJob(const Account& account, qint64 objects, bool destructive = true,
            QList<s3::Feature> features = {})
        : Job(account, QStringLiteral("bkt")),
          m_objects(objects),
          m_destructive(destructive),
          m_features(std::move(features))
    {
    }
    int planned = 0;
    int executed = 0;
    int verified = 0;
    bool mutating = true;
    bool pausable = false;
    bool holdExecution = false;   // stay in Executing until release()
    int pauses = 0;
    int resumes = 0;
    QStringList bucketList;   // non-empty: buckets() (a copy across buckets)

    [[nodiscard]] QString title() const override { return QStringLiteral("Test job"); }
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override { return m_features; }
    [[nodiscard]] bool isDestructive() const override { return m_destructive; }
    [[nodiscard]] bool isMutating() const override { return mutating; }
    [[nodiscard]] bool canPause() const override { return pausable; }
    [[nodiscard]] QStringList buckets() const override { return bucketList.isEmpty() ? Job::buckets() : bucketList; }

    void release()
    {
        setBytes_(100, 100);
        executed_();
    }

protected:
    void plan() override
    {
        ++planned;
        QMetaObject::invokeMethod(this, [this] {
            Preview p;
            p.action = QStringLiteral("Delete");
            p.scope = QStringLiteral("s3://bkt/photos/");
            p.objects = m_objects;
            p.samples = {QStringLiteral("photos/a.jpg")};
            setPreview_(p);
        }, Qt::QueuedConnection);
    }
    void onPause() override { ++pauses; }
    void onResume() override { ++resumes; }
    void execute() override
    {
        ++executed;
        executedEver.insert(this);
        if (holdExecution) {
            setBytes_(10, 100);
            return;
        }
        QMetaObject::invokeMethod(this, [this] {
            Journal::Record r = record_(QStringLiteral("DeleteObjects"), QStringLiteral("photos/a.jpg"));
            r.httpStatus = 200;
            r.requestId = QStringLiteral("REQ1");
            journal_({r});
            setProgress_(1, 1);
            executed_();
        }, Qt::QueuedConnection);
    }
    void verify() override
    {
        ++verified;
        Report r;
        r.succeeded = m_objects;
        r.verified = true;
        r.verification = QStringLiteral("nothing left");
        finish_(r);
    }

private:
    qint64 m_objects;
    bool m_destructive;
    QList<s3::Feature> m_features;
};

} // namespace

class TestOperationController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_theme.apply(QStringLiteral("Light"));
    }

    void init()
    {
        executedEver.clear();   // addresses of deleted jobs get reused
        m_h = std::make_unique<AccountHarness>(QStringLiteral("Prod"));
        QVERIFY(m_h->ok());
        m_failProbes = false;
        m_h->server->setFallbackHandler([this](const RecordedRequest& r) {
            if (m_failProbes)
                return FakeResponse::s3Error(403, "InvalidAccessKeyId");
            if (r.path == "/")
                return FakeResponse::ok(kBuckets);
            if (r.hasQueryItem("lifecycle"))
                return FakeResponse::s3Error(501, "NotImplemented");
            if (r.hasQueryItem("list-type"))
                return FakeResponse::ok("<ListBucketResult><Name>bkt</Name><IsTruncated>false</IsTruncated></ListBucketResult>");
            if (!r.query.isEmpty())
                return FakeResponse::s3Error(404, "NoSuchThing");
            return FakeResponse();
        });
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY(m_caps->isLoaded(m_h->account.id));
        // The server probe started with the session must be over before a
        // test counts requests (slow runners: it arrived mid-test).
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->serverSet(m_h->account.id).probedAt.isValid()
                                     && !m_caps->isProbing(m_h->account.id), 10000);
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

    void theWholeWay()
    {
        auto* job = new TestJob(m_h->account, 3);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QCOMPARE(job->planned, 0);   // never inside start()
        QVERIFY(preview.wait(5000));
        QCOMPARE(job->phase(), Job::Phase::AwaitingConfirmation);
        QCOMPARE(job->executed, 0);
        QCOMPARE(m_ops->jobs().size(), 1);

        job->confirm();
        QVERIFY(done.wait(10000));   // the bucket is probed first: destructive
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->executed, 1);
        QCOMPARE(job->verified, 1);
        QVERIFY(m_caps->bucketSet(m_h->account.id, QStringLiteral("bkt")).has_value());
        QVERIFY(m_log->entries().last().text.contains(QLatin1String("Test job")));

        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject line = QJsonDocument::fromJson(f.readLine()).object();
        QCOMPARE(line.value(QStringLiteral("op")).toString(), QStringLiteral("DeleteObjects"));
        QCOMPARE(line.value(QStringLiteral("key")).toString(), QStringLiteral("photos/a.jpg"));
        QCOMPARE(line.value(QStringLiteral("requestId")).toString(), QStringLiteral("REQ1"));
        QCOMPARE(line.value(QStringLiteral("environment")).toString(), QStringLiteral("production"));
        QCOMPARE(line.value(QStringLiteral("result")).toString(), QStringLiteral("ok"));
        QCOMPARE(line.value(QStringLiteral("job")).toString(), job->id());
        QTRY_VERIFY(m_ops->jobs().isEmpty());
    }

    void aReadOnlyAccountChangesNothing()
    {
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        auto* job = new TestJob(ro, 1);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(5000));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QCOMPARE(job->planned, 0);
        QVERIFY(job->errorText().contains(QLatin1String("read-only")));
        QVERIFY(!m_ops->check(ro.id, QStringLiteral("bkt"), {}, false).allowed);
    }

    void aReadOnlyAccountMayStillRead()
    {
        // A download: allowed on a read-only account, writes nothing remote.
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        auto* job = new TestJob(ro, 1, false);
        job->mutating = false;
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        job->confirm();
        QVERIFY(done.wait(5000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(m_ops->check(ro.id, QStringLiteral("bkt"), {}, false, false).allowed);
        QVERIFY(!m_ops->check(ro.id, QStringLiteral("bkt"), {}, false, true).allowed);
    }

    void pauseAndResumeWhileExecuting()
    {
        auto* job = new TestJob(m_h->account, 1, false);
        job->pausable = true;
        job->holdExecution = true;
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy paused(job, &Job::pausedChanged);
        QSignalSpy done(job, &Job::finished);
        job->pause();   // not yet executing: no effect
        QVERIFY(!job->isPaused());
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        job->confirm();
        QTRY_COMPARE(job->phase(), Job::Phase::Executing);
        QCOMPARE(job->bytesDone(), 10);
        QCOMPARE(job->bytesTotal(), 100);
        job->pause();
        job->pause();
        QVERIFY(job->isPaused());
        QCOMPARE(job->pauses, 1);
        job->resume();
        QVERIFY(!job->isPaused());
        QCOMPARE(job->resumes, 1);
        QCOMPARE(paused.size(), 2);
        job->release();   // verifies and finishes right away
        QCOMPARE(done.size(), 1);
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);

        // Jobs that can't pause ignore it.
        auto* plain = new TestJob(m_h->account, 1, false);
        plain->holdExecution = true;
        QSignalSpy plainDone(plain, &Job::finished);
        m_ops->start(plain);
        QVERIFY(preview.wait(5000));
        plain->confirm();
        QTRY_COMPARE(plain->phase(), Job::Phase::Executing);
        plain->pause();
        QVERIFY(!plain->isPaused());
        plain->release();
        QCOMPARE(plainDone.size(), 1);
    }

    void readOnlySwitchedOnAfterThePreviewStillStops()
    {
        auto* job = new TestJob(m_h->account, 1);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        job->confirm();   // the gate is checked again, synchronously
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QCOMPARE(job->executed, 0);
    }

    void unknownNeverUnlocksADestructiveJob()
    {
        // Nothing probed: a destructive job needing a feature is refused,
        // a harmless one may run unverified.
        const QUuid id = m_h->account.id;
        QVERIFY(!m_ops->check(id, QStringLiteral("other"), {s3::Feature::Lifecycle}, true).allowed);
        QVERIFY(m_ops->check(id, QStringLiteral("other"), {s3::Feature::Lifecycle}, false).allowed);

        auto* job = new TestJob(m_h->account, 1, true, {s3::Feature::Lifecycle});
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(5000));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QVERIFY(!job->errorText().isEmpty());
    }

    void anUnsupportedFeatureIsRefusedWithItsReason()
    {
        m_caps->bucketOpened(m_h->account.id, QStringLiteral("bkt"));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_h->account.id, QStringLiteral("bkt")).has_value(), 10000);
        const OperationController::Check c =
            m_ops->check(m_h->account.id, QStringLiteral("bkt"), {s3::Feature::Lifecycle}, false);
        QVERIFY(!c.allowed);
        QVERIFY(c.reason.contains(QLatin1String("501")));
    }

    // A job across buckets (a copy) is checked on each of them.
    void everyBucketOfAJobIsChecked()
    {
        m_caps->bucketOpened(m_h->account.id, QStringLiteral("bkt"));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_h->account.id, QStringLiteral("bkt")).has_value(), 10000);
        auto* fine = new TestJob(m_h->account, 1, false, {s3::Feature::Lifecycle});
        fine->bucketList = {QStringLiteral("other")};   // not probed: allowed unverified (not destructive)
        QSignalSpy fineDone(fine, &Job::finished);
        connect(m_ops.get(), &OperationController::previewNeeded, fine, [fine](Job* j) {
            if (j == fine)
                j->confirm();
        });
        m_ops->start(fine);
        QVERIFY(fineDone.wait(10000));
        QCOMPARE(fine->outcome(), Job::Outcome::Succeeded);

        auto* blocked = new TestJob(m_h->account, 1, false, {s3::Feature::Lifecycle});
        blocked->bucketList = {QStringLiteral("other"), QStringLiteral("bkt")};   // bkt: 501
        QSignalSpy done(blocked, &Job::finished);
        m_ops->start(blocked);
        QVERIFY(done.wait(10000));
        QCOMPARE(blocked->outcome(), Job::Outcome::Refused);
        QVERIFY(blocked->errorText().contains(QLatin1String("501")));
        QCOMPARE(blocked->planned, 0);
    }

    // Destructive: the capabilities of every bucket are made fresh first.
    void aDestructiveJobProbesEveryBucket()
    {
        auto* job = new TestJob(m_h->account, 1, true);
        job->bucketList = {QStringLiteral("bkt"), QStringLiteral("second")};
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        job->confirm();
        QVERIFY(done.wait(15000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(m_caps->bucketSet(m_h->account.id, QStringLiteral("bkt")).has_value());
        QVERIFY(m_caps->bucketSet(m_h->account.id, QStringLiteral("second")).has_value());
    }

    // Tag / ACL writes need write support the profile confirms — a read
    // probe doesn't do (generic profile: unknown, refused with the reason).
    void tagAndAclWritesNeedConfirmedSupport()
    {
        const OperationController::Check c =
            m_ops->check(m_h->account.id, QStringLiteral("bkt"), {s3::Feature::ObjectTaggingWrite}, true);
        QVERIFY(!c.allowed);
        QVERIFY2(c.reason.contains(QLatin1String("tags and ACLs")), qPrintable(c.reason));
    }

    void cancellingBeforeConfirmationSendsNothing()
    {
        auto* job = new TestJob(m_h->account, 1);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        const int requests = m_h->server->requestCount();
        job->cancel();
        QCOMPARE(done.size(), 1);
        QCOMPARE(job->outcome(), Job::Outcome::Cancelled);
        QCOMPARE(job->executed, 0);
        QTest::qWait(100);
        QCOMPARE(m_h->server->requestCount(), requests);
    }

    void cancellingWhileTheBucketIsProbedSendsNothing()
    {
        auto* job = new TestJob(m_h->account, 1);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        job->confirm();   // starts the probe
        job->cancel();
        QCOMPARE(job->outcome(), Job::Outcome::Cancelled);
        const QPointer<TestJob> guard(job);
        QTest::qWait(500);   // the probe's answer arrives: nothing may execute
        QVERIFY(!guard);     // deleted after finishing, never executed
        QVERIFY(executedEver.isEmpty());
    }

    void aFailedProbeBeforeExecutingIsNoGreenLight()
    {
        auto* job = new TestJob(m_h->account, 1);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        m_failProbes = true;
        job->confirm();
        QVERIFY(done.wait(10000));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QCOMPARE(job->executed, 0);
    }

    void largeScopesAreTypedToConfirm()
    {
        auto* job = new TestJob(m_h->account, OperationController::kTypeToConfirmAbove + 1);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        QCOMPARE(job->preview().typeToConfirm, QStringLiteral("photos"));
        job->cancel();

        auto* small = new TestJob(m_h->account, 3);
        preview.clear();
        m_ops->start(small);
        QVERIFY(preview.wait(5000));
        QVERIFY(small->preview().typeToConfirm.isEmpty());
        small->cancel();
    }

    void thePreviewDialogAsksTwiceForProduction()
    {
        auto* job = new TestJob(m_h->account, OperationController::kTypeToConfirmAbove + 5);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));

        OperationPreviewDialog dlg(job);
        dlg.show();
        QVERIFY(dlg.productionCheck());   // the harness account is Production
        QVERIFY(dlg.typedConfirmation());
        QVERIFY(!dlg.confirmButton()->isEnabled());
        dlg.productionCheck()->setChecked(true);
        QVERIFY(!dlg.confirmButton()->isEnabled());
        dlg.typedConfirmation()->setText(QStringLiteral("photo"));
        QVERIFY(!dlg.confirmButton()->isEnabled());
        dlg.typedConfirmation()->setText(QStringLiteral("photos"));
        QVERIFY(dlg.confirmButton()->isEnabled());
        dlg.confirmButton()->click();
        QVERIFY(done.wait(10000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
    }

    void closingThePreviewCancels()
    {
        Account dev = m_h->account;
        dev.environment = Environment::Development;
        QVERIFY(m_h->accounts->save(dev));
        auto* job = new TestJob(dev, 2);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        m_ops->start(job);
        QVERIFY(preview.wait(5000));
        OperationPreviewDialog dlg(job);
        QVERIFY(!dlg.productionCheck());
        QVERIFY(dlg.confirmButton()->isEnabled());
        QPointer<TestJob> guard(job);
        dlg.reject();
        QCOMPARE(guard->outcome(), Job::Outcome::Cancelled);
        QCOMPARE(guard->executed, 0);
    }

private:
    ThemeManager m_theme;
    std::unique_ptr<AccountHarness> m_h;
    bool m_failProbes = false;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
};

QTEST_MAIN(TestOperationController)
#include "test_operation_controller.moc"
