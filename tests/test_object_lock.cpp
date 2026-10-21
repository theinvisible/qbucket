// ObjectLockJob (retention set / extended / shortened only with the bypass /
// removed only with it; COMPLIANCE only extended and typed; legal hold on
// and off; pinned to the version shown) and the Lock tab of the DetailPane
// (shown on buckets with object lock only, read while shown, buttons say
// why they are off).

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/ObjectLockJob.h"
#include "controllers/OperationController.h"
#include "models/ObjectTableModel.h"
#include "support/AccountHarness.h"
#include "ui/DetailPane.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QComboBox>
#include <QDateTimeEdit>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");
const QString kKey = QStringLiteral("docs/contract.pdf");

} // namespace

class TestObjectLock : public QObject {
    Q_OBJECT

private:
    ThemeManager m_theme;
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
    QList<Job::Preview> m_previews;
    bool m_bypass = false;
    QString m_v1;
    QString m_v2;

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

    std::pair<Job::Outcome, QString> run(Job* job)
    {
        std::optional<std::pair<Job::Outcome, QString>> out;
        connect(job, &Job::finished, this, [&out, job] {
            out = {job->outcome(), job->errorText() + QLatin1Char('|') + job->report().verification};
        });
        m_ops->start(job);
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 20000))
            return {Job::Outcome::None, QStringLiteral("timeout")};
        return *out;
    }

    s3::ObjectRetention retentionOf(const QString& version)
    {
        std::optional<s3::Result<s3::ObjectRetention>> out;
        (*m_h->sessions->session(m_h->account.id))
            ->getObjectRetention(kBucket, kKey, version, this, [&out](const s3::Result<s3::ObjectRetention>& r) { out = r; });
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 10000) || !*out)
            return {};
        return out->value();
    }

    // The account's provider, then the bucket probed.
    void setUp(const QString& provider = QStringLiteral("aws"))
    {
        useProvider(provider);
        probe();
    }

    QDateTime inDays(int days) const { return QDateTime::currentDateTimeUtc().addDays(days); }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_objects->setObjectLockEnabled(kBucket);
        m_v1 = m_objects->putObject(kBucket, kKey, "first");
        m_v2 = m_objects->putObject(kBucket, kKey, "second");
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_previews.clear();
        m_bypass = false;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            job->setBypassGovernance(m_bypass);
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
        m_objects.reset();
        m_h.reset();
    }

    void setExtendShortenRemove()
    {
        setUp();
        auto [outcome, text] = run(ObjectLockJob::retention(m_h->account, kBucket, kKey, {}, s3::ObjectLockMode::Governance,
                                                             inDays(2)));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QVERIFY2(text.contains(QLatin1String("read back")), qPrintable(text));
        QCOMPARE(retentionOf(m_v2).mode, std::optional(s3::ObjectLockMode::Governance));
        QVERIFY(!retentionOf(m_v1).mode);   // pinned to the current version
        QVERIFY(!m_previews.last().bypassOffer);

        // Longer: no bypass needed.
        QCOMPARE(run(ObjectLockJob::retention(m_h->account, kBucket, kKey, {}, s3::ObjectLockMode::Governance,
                                              inDays(5))).first,
                 Job::Outcome::Succeeded);
        // Shorter: offered the bypass; without it nothing is sent.
        const int puts = m_objects->count(QStringLiteral("PutObjectRetention"));
        std::tie(outcome, text) = run(ObjectLockJob::retention(m_h->account, kBucket, kKey, {},
                                                               s3::ObjectLockMode::Governance, inDays(1)));
        QCOMPARE(outcome, Job::Outcome::Failed);
        QVERIFY(m_previews.last().bypassOffer);
        QVERIFY(text.contains(QLatin1String("bypass")));
        QCOMPARE(m_objects->count(QStringLiteral("PutObjectRetention")), puts);
        m_bypass = true;
        QCOMPARE(run(ObjectLockJob::retention(m_h->account, kBucket, kKey, {}, s3::ObjectLockMode::Governance,
                                              inDays(1))).first,
                 Job::Outcome::Succeeded);
        QVERIFY(retentionOf(m_v2).retainUntil < inDays(2));
        // Removing, with the bypass.
        QCOMPARE(run(ObjectLockJob::removeRetention(m_h->account, kBucket, kKey, {})).first, Job::Outcome::Succeeded);
        QVERIFY(!retentionOf(m_v2).mode);
        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray journal = f.readAll();
        QVERIFY(journal.contains("PutObjectRetention"));
        QVERIFY(journal.contains("\"bypassGovernance\":true"));
    }

    void complianceOnlyExtends()
    {
        setUp();
        auto [outcome, text] = run(ObjectLockJob::retention(m_h->account, kBucket, kKey, m_v1,
                                                             s3::ObjectLockMode::Compliance, inDays(2)));
        QCOMPARE(outcome, Job::Outcome::Succeeded);
        QCOMPARE(m_previews.last().typeToConfirm, QStringLiteral("contract.pdf"));   // nobody can take it back
        QVERIFY(!m_previews.last().warnings.isEmpty());
        QCOMPARE(retentionOf(m_v1).mode, std::optional(s3::ObjectLockMode::Compliance));
        m_bypass = true;   // never helps
        std::tie(outcome, text) = run(ObjectLockJob::retention(m_h->account, kBucket, kKey, m_v1,
                                                               s3::ObjectLockMode::Compliance, inDays(1)));
        QCOMPARE(outcome, Job::Outcome::Failed);
        QVERIFY(text.contains(QLatin1String("only be extended")));
        std::tie(outcome, text) = run(ObjectLockJob::removeRetention(m_h->account, kBucket, kKey, m_v1));
        QCOMPARE(outcome, Job::Outcome::Failed);
        std::tie(outcome, text) = run(ObjectLockJob::retention(m_h->account, kBucket, kKey, m_v1,
                                                               s3::ObjectLockMode::Governance, inDays(9)));
        QCOMPARE(outcome, Job::Outcome::Failed);
        QCOMPARE(run(ObjectLockJob::retention(m_h->account, kBucket, kKey, m_v1, s3::ObjectLockMode::Compliance,
                                              inDays(3))).first,
                 Job::Outcome::Succeeded);
    }

    void legalHoldOnAndOff()
    {
        setUp();
        QCOMPARE(run(ObjectLockJob::legalHold(m_h->account, kBucket, kKey, m_v1, true)).first, Job::Outcome::Succeeded);
        QVERIFY(m_objects->versions(kBucket, kKey).last().object.headers.value(QStringLiteral("x-amz-object-lock-legal-hold"))
                == QLatin1String("ON"));
        // Already on: nothing to do, nothing to confirm.
        const qsizetype previews = m_previews.size();
        QCOMPARE(run(ObjectLockJob::legalHold(m_h->account, kBucket, kKey, m_v1, true)).first, Job::Outcome::Succeeded);
        QCOMPARE(m_previews.size(), previews);
        QCOMPARE(run(ObjectLockJob::legalHold(m_h->account, kBucket, kKey, m_v1, false)).first, Job::Outcome::Succeeded);
        QVERIFY(!m_objects->versions(kBucket, kKey).last().object.headers.contains(QStringLiteral("x-amz-object-lock-legal-hold")));
    }

    void unconfirmedWritesAreRefused()
    {
        setUp(QStringLiteral("generic"));
        const auto [outcome, text] = run(ObjectLockJob::legalHold(m_h->account, kBucket, kKey, {}, true));
        QCOMPARE(outcome, Job::Outcome::Refused);
        QVERIFY(text.contains(QLatin1String("not confirmed")));
    }

    void theLockTab()
    {
        setUp();
        m_objects->setRetention(kBucket, kKey, m_v2, s3::ObjectLockMode::Governance, inDays(3));
        ObjectTableModel model(m_h->sessions.get());
        model.setLocation(m_h->account.id, kBucket, QStringLiteral("docs/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), ObjectTableModel::State::Complete, 10000);
        DetailPane pane(m_h->sessions.get());
        pane.setControllers(m_ops.get(), m_caps.get());
        pane.show();
        pane.setSelection(m_h->account.id, kBucket, {model.index(model.rowOfKey(kKey), 0)});
        QVERIFY(pane.tabs()->isTabVisible(DetailPane::LockTab));
        QSignalSpy head(&pane, &DetailPane::headFinished);
        QVERIFY(head.wait(10000));
        QSignalSpy loaded(&pane, &DetailPane::lockLoaded);
        pane.tabs()->setCurrentIndex(DetailPane::LockTab);
        QVERIFY(loaded.size() >= 1 || loaded.wait(10000));
        QTRY_VERIFY(pane.retentionLabel()->text().startsWith(QLatin1String("GOVERNANCE")));
        QCOMPARE(pane.legalHoldLabel()->text(), QStringLiteral("off"));
        QVERIFY(pane.setRetentionButton()->isEnabled());
        QVERIFY(pane.removeRetentionButton()->isEnabled());

        loaded.clear();
        pane.legalHoldButton()->click();
        QVERIFY(loaded.wait(20000));
        QVERIFY(pane.legalHoldLabel()->text().startsWith(QLatin1String("on")));
        QCOMPARE(pane.legalHoldButton()->text(), QStringLiteral("Lift Legal Hold…"));
    }

    void noLockTabWithoutObjectLock()
    {
        setUp();
        m_objects->createBucket(QStringLiteral("plain"));
        m_objects->putObject(QStringLiteral("plain"), QStringLiteral("a.txt"), "a");
        m_caps->bucketOpened(m_h->account.id, QStringLiteral("plain"));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_h->account.id, QStringLiteral("plain")).has_value(), 10000);
        ObjectTableModel model(m_h->sessions.get());
        model.setLocation(m_h->account.id, QStringLiteral("plain"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), ObjectTableModel::State::Complete, 10000);
        DetailPane pane(m_h->sessions.get());
        pane.setControllers(m_ops.get(), m_caps.get());
        pane.setSelection(m_h->account.id, QStringLiteral("plain"), {model.index(0, 0)});
        QVERIFY(!pane.tabs()->isTabVisible(DetailPane::LockTab));
    }
};

QTEST_MAIN(TestObjectLock)
#include "test_object_lock.moc"
