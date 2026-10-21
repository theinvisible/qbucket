// BucketPropertiesDialog: the bucket's versioning read fresh, enabled
// through the OperationController (SetBucketVersioningJob), and what can't
// be done says why.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Job.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/AccountHarness.h"
#include "ui/BucketPropertiesDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestBucketPropertiesDialog : public QObject {
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
        m_caps->bucketOpened(m_h->account.id, QStringLiteral("bkt"));
        QTRY_VERIFY_WITH_TIMEOUT(!m_caps->isProbing(m_h->account.id, QStringLiteral("bkt"))
                                     && m_caps->bucketSet(m_h->account.id, QStringLiteral("bkt")),
                                 10000);
    }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(QStringLiteral("bkt"));
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        connect(m_ops.get(), &OperationController::previewNeeded, this, [](Job* job) { job->confirm(); });
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

    void enablesVersioningThroughTheGate()
    {
        useProvider(QStringLiteral("aws"));
        probe();
        BucketPropertiesDialog dlg(m_h->account.id, QStringLiteral("bkt"), m_h->sessions.get(), m_ops.get(), m_caps.get());
        QSignalSpy loaded(&dlg, &BucketPropertiesDialog::versioningLoaded);
        QVERIFY(loaded.size() == 1 || loaded.wait(10000));
        QCOMPARE(dlg.versioningLabel()->text(), QStringLiteral("Never enabled"));
        QVERIFY(dlg.enableButton()->isEnabled());
        QVERIFY(!dlg.suspendButton()->isEnabled());   // nothing to suspend

        loaded.clear();
        dlg.enableButton()->click();
        QVERIFY(!dlg.enableButton()->isEnabled());   // under way
        QVERIFY(loaded.wait(20000));
        QCOMPARE(m_objects->versioning(QStringLiteral("bkt")), QStringLiteral("Enabled"));
        QCOMPARE(dlg.versioningLabel()->text(), QStringLiteral("Enabled"));
        QVERIFY(!dlg.enableButton()->isEnabled());
        QVERIFY(dlg.suspendButton()->isEnabled());
    }

    void unconfirmedWritesSayWhy()
    {
        useProvider(QStringLiteral("generic"));
        probe();
        m_objects->setVersioning(QStringLiteral("bkt"), QStringLiteral("Enabled"));
        BucketPropertiesDialog dlg(m_h->account.id, QStringLiteral("bkt"), m_h->sessions.get(), m_ops.get(), m_caps.get());
        dlg.showPage(BucketPropertiesDialog::VersioningPage);
        QSignalSpy loaded(&dlg, &BucketPropertiesDialog::versioningLoaded);
        QVERIFY(loaded.size() == 1 || loaded.wait(10000));
        QCOMPARE(dlg.versioningLabel()->text(), QStringLiteral("Enabled"));
        QVERIFY(!dlg.suspendButton()->isEnabled());
        const auto* reason = dlg.findChildren<QLabel*>(QStringLiteral("hint")).last();
        QVERIFY(reason->text().contains(QLatin1String("not confirmed")));
    }
};

QTEST_MAIN(TestBucketPropertiesDialog)
#include "test_bucket_properties_dialog.moc"
