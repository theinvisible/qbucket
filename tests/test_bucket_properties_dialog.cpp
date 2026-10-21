// BucketPropertiesDialog: the bucket's versioning read fresh, enabled
// through the OperationController (SetBucketVersioningJob), and what can't
// be done says why. With QBUCKET_SCREENSHOT_DIR: the devlog's M5 pictures
// (versions view, Versions tab, properties, the permanent-delete preview).

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Job.h"
#include "controllers/BrowseController.h"
#include "controllers/DeleteVersionsJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "models/ObjectTableModel.h"
#include "support/AccountHarness.h"
#include "ui/BucketPropertiesDialog.h"
#include "ui/DetailPane.h"
#include "ui/ObjectBrowser.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <QTranslator>
#include <QTreeWidget>
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

    // QBUCKET_SCREENSHOT_DIR (with a real platform): versions-view.png,
    // details-versions.png, bucket-properties.png, delete-versions-preview.png.
    void screenshot()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        QTranslator plurals;   // as the app does
        if (plurals.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")))
            QCoreApplication::installTranslator(&plurals);
        m_theme.apply(QStringLiteral("Dark"));
        QDir().mkpath(dir);
        useProvider(QStringLiteral("aws"));
        const QString b = QStringLiteral("bkt");
        m_objects->setVersioning(b, QStringLiteral("Enabled"));
        for (const char* text : {"<h1>Spring</h1>", "<h1>Summer sale</h1>", "<h1>Autumn</h1>"})
            m_objects->putObject(b, QStringLiteral("site/index.html"), QByteArray(text).repeated(300), QStringLiteral("text/html"));
        m_objects->putObject(b, QStringLiteral("site/logo.svg"), QByteArray(5200, 's'), QStringLiteral("image/svg+xml"));
        m_objects->putObject(b, QStringLiteral("site/old-banner.png"), QByteArray(184000, 'p'), QStringLiteral("image/png"));
        m_objects->deleteObject(b, QStringLiteral("site/old-banner.png"));
        m_objects->putObject(b, QStringLiteral("site/styles.css"), QByteArray(9100, 'c'), QStringLiteral("text/css"));
        m_objects->putObject(b, QStringLiteral("site/styles.css"), QByteArray(9400, 'c'), QStringLiteral("text/css"));
        m_objects->putObject(b, QStringLiteral("site/assets/app.js"), QByteArray(64000, 'j'));
        probe();

        // The versions view.
        ObjectBrowser browser(m_h->accounts.get(), m_h->sessions.get());
        browser.resize(860, 300);
        browser.setMode(ObjectTableModel::Mode::Versions);
        browser.show();
        browser.navigation()->navigate({m_h->account.id, b, QStringLiteral("site/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), ObjectTableModel::State::Complete, 10000);
        browser.view()->selectRow(2);
        QTest::qWait(300);
        QVERIFY(browser.grab().save(QDir(dir).filePath(QStringLiteral("versions-view.png"))));

        // The Versions tab of index.html, with its earlier version shown.
        DetailPane pane(m_h->sessions.get());
        pane.setControllers(m_ops.get(), m_caps.get());
        pane.resize(440, 460);
        pane.show();
        QSignalSpy versions(&pane, &DetailPane::versionsLoaded);
        pane.tabs()->setCurrentIndex(DetailPane::VersionsTab);
        // The row after a key's newest entry is its previous version.
        const int older = browser.model()->rowOfKey(QStringLiteral("site/index.html")) + 1;
        pane.setSelection(m_h->account.id, b, {browser.model()->index(older, 0)});
        QVERIFY(versions.size() == 1 || versions.wait(10000));
        QTest::qWait(300);
        QVERIFY(pane.grab().save(QDir(dir).filePath(QStringLiteral("details-versions.png"))));

        // The bucket's properties, on the Versioning page.
        BucketPropertiesDialog dlg(m_h->account.id, b, m_h->sessions.get(), m_ops.get(), m_caps.get());
        dlg.showPage(BucketPropertiesDialog::VersioningPage);
        QSignalSpy loaded(&dlg, &BucketPropertiesDialog::versioningLoaded);
        dlg.show();
        QVERIFY(loaded.size() == 1 || loaded.wait(10000));
        QTest::qWait(300);
        QVERIFY(dlg.grab().save(QDir(dir).filePath(QStringLiteral("bucket-properties.png"))));
        dlg.close();

        // Every version of the folder, for good: the preview.
        disconnect(m_ops.get(), &OperationController::previewNeeded, this, nullptr);
        OperationPreviewDialog* preview = nullptr;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [&preview](Job* job) {
            preview = new OperationPreviewDialog(job);
            preview->setAttribute(Qt::WA_DeleteOnClose);
            preview->show();
        });
        m_ops->start(DeleteVersionsJob::everything(m_h->account, b, {{QStringLiteral("site/"), true}}, m_ops->planDir()));
        QTRY_VERIFY_WITH_TIMEOUT(preview != nullptr, 10000);
        QTest::qWait(500);
        QVERIFY(preview->grab().save(QDir(dir).filePath(QStringLiteral("delete-versions-preview.png"))));
        preview->reject();
        m_theme.apply(QStringLiteral("Light"));
    }
};

QTEST_MAIN(TestBucketPropertiesDialog)
#include "test_bucket_properties_dialog.moc"
