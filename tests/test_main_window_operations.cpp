#include "controllers/BrowseController.h"
#include "controllers/SettingsController.h"
#include "controllers/TransferJobStore.h"
#include "support/AccountHarness.h"
#include "models/ObjectSelection.h"
#include "support/FakeBucket.h"
#include "ui/MainWindow.h"
#include "ui/ObjectBrowser.h"
#include "ui/EditMetadataDialog.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/ThemeManager.h"

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableView>
#include <QClipboard>
#include <QGuiApplication>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;
using qbucket::testing::FakeBucket;

// The mutation path end to end through the real main window: actions,
// gate, preview dialog, job, refresh.
class TestMainWindowOperations : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Before any settings object: never the user's own settings or data.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("qbucket-test-ops"));
        QCoreApplication::setOrganizationName(QStringLiteral("qbucket-test"));
        m_settings = std::make_unique<SettingsController>();
        m_theme = std::make_unique<ThemeManager>();
    }

    void init()
    {
        m_h = std::make_unique<AccountHarness>(QStringLiteral("Prod"));
        QVERIFY(m_h->ok());
        Account a = m_h->account;
        a.providerId = QStringLiteral("aws");   // BatchDelete known
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        m_bucket = FakeBucket();
        m_bucket.add(QStringLiteral("doomed/"));
        m_bucket.add(QStringLiteral("doomed/a"), 3);
        m_bucket.add(QStringLiteral("doomed/b/c"), 4);
        m_bucket.add(QStringLiteral("keep.txt"), 5);
        m_bucket.sort();
        m_h->server->setFallbackHandler([this](const RecordedRequest& r) {
            if (r.method == "GET" && !r.query.isEmpty() && !r.hasQueryItem("list-type"))
                return r.hasQueryItem("versioning") ? FakeResponse::ok("<VersioningConfiguration/>")
                                                    : FakeResponse::s3Error(501, "NotImplemented");
            if (r.path == "/")
                return FakeResponse::ok("<ListAllMyBucketsResult><Buckets><Bucket><Name>bkt</Name>"
                                        "<CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                                        "</Buckets></ListAllMyBucketsResult>");
            return m_bucket.handle(r);
        });
        m_window = std::make_unique<MainWindow>(m_settings.get(), m_theme.get(), m_h->accounts.get(), m_h->secrets.get());
        m_window->resize(1100, 700);
        m_window->show();
        m_browser = m_window->findChild<ObjectBrowser*>();
        QVERIFY(m_browser);
        m_browser->openBucket(m_h->account.id, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
    }

    void cleanup()
    {
        m_window.reset();
        m_h.reset();
    }

    void deleteAFolderFromTheList()
    {
        QCOMPARE(m_browser->model()->rowCount(), 2);   // doomed, keep.txt
        m_browser->view()->setFocus();
        m_browser->view()->selectRow(0);
        QAction* del = m_browser->deleteAction();
        QTRY_VERIFY(del->isEnabled());
        del->trigger();

        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<OperationPreviewDialog*>() != nullptr, 10000);
        auto* dlg = m_window->findChild<OperationPreviewDialog*>();
        QVERIFY(dlg);
        QTRY_VERIFY(dlg->isVisible());
        QVERIFY(!dlg->confirmButton()->isEnabled());   // production: tick first
        dlg->productionCheck()->setChecked(true);
        dlg->confirmButton()->click();

        QTRY_VERIFY_WITH_TIMEOUT(!m_bucket.contains(QStringLiteral("doomed/a")), 10000);
        QVERIFY(m_bucket.contains(QStringLiteral("keep.txt")));
        // Listed afresh afterwards.
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->rowCount(), 1, 10000);
        QCOMPARE(m_browser->model()->index(0, 0).data().toString(), QStringLiteral("keep.txt"));
    }

    void newFolderFromTheMenu()
    {
        QAction* create = nullptr;
        for (QAction* a : m_window->findChildren<QAction*>()) {
            if (a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N))
                create = a;
        }
        QVERIFY(create);
        if (!create)
            return;   // (for the static analyzer)
        QTRY_VERIFY(create->isEnabled());
        create->trigger();
        QTRY_VERIFY(m_window->findChild<QInputDialog*>() != nullptr);
        auto* ask = m_window->findChild<QInputDialog*>();
        QVERIFY(ask);
        ask->setTextValue(QStringLiteral("fresh"));
        ask->accept();
        QTRY_VERIFY_WITH_TIMEOUT(m_bucket.contains(QStringLiteral("fresh/")), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->rowOfKey(QStringLiteral("fresh/")) >= 0, true, 10000);
    }

    void renameFromTheList()
    {
        m_browser->view()->setFocus();
        m_browser->view()->selectRow(1);   // keep.txt
        QAction* rename = m_browser->renameAction();
        QTRY_VERIFY(rename->isEnabled());
        rename->trigger();
        QTRY_VERIFY(m_window->findChild<QInputDialog*>() != nullptr);
        auto* ask = m_window->findChild<QInputDialog*>();
        QVERIFY(ask);
        QCOMPARE(ask->textValue(), QStringLiteral("keep.txt"));
        ask->setTextValue(QStringLiteral("kept.txt"));
        ask->accept();

        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<OperationPreviewDialog*>() != nullptr, 10000);
        auto* dlg = m_window->findChild<OperationPreviewDialog*>();
        QVERIFY(dlg);
        dlg->productionCheck()->setChecked(true);
        dlg->confirmButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_bucket.contains(QStringLiteral("kept.txt")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!m_bucket.contains(QStringLiteral("keep.txt")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_browser->model()->rowOfKey(QStringLiteral("kept.txt")) >= 0, 10000);
        // Two rows selected: one name can't go to both.
        m_browser->view()->selectAll();
        QVERIFY(!rename->isEnabled());
    }

    // Cut keep.txt, open the folder, paste: a move through the preview; the
    // clipboard lets go of what was moved.
    void cutAndPasteMovesAnObject()
    {
        m_browser->view()->setFocus();
        m_browser->view()->selectRow(1);   // keep.txt
        QTRY_VERIFY(m_browser->cutAction()->isEnabled());
        m_browser->cutAction()->trigger();
        QVERIFY(ObjectSelection::fromMimeData(QGuiApplication::clipboard()->mimeData()));
        m_browser->navigation()->navigate({m_h->account.id, QStringLiteral("bkt"), QStringLiteral("doomed/")});
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
        QTRY_VERIFY2(m_browser->pasteAction()->isEnabled(), qPrintable(m_browser->pasteAction()->toolTip()));
        m_browser->pasteAction()->trigger();

        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<OperationPreviewDialog*>() != nullptr, 10000);
        auto* dlg = m_window->findChild<OperationPreviewDialog*>();
        QVERIFY(dlg);
        dlg->productionCheck()->setChecked(true);   // a move deletes: production asks twice
        dlg->confirmButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_bucket.contains(QStringLiteral("doomed/keep.txt")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!m_bucket.contains(QStringLiteral("keep.txt")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!ObjectSelection::fromMimeData(QGuiApplication::clipboard()->mimeData()), 10000);
        // The folder shown is the target: listed afresh.
        QTRY_VERIFY_WITH_TIMEOUT(m_browser->model()->rowOfKey(QStringLiteral("doomed/keep.txt")) >= 0, 10000);
    }

    // Edit Metadata on one object: its values, read fresh, in the dialog.
    void editMetadataOpensWithTheObjectsValues()
    {
        m_browser->view()->setFocus();
        m_browser->view()->selectRow(1);   // keep.txt
        QTRY_VERIFY(m_browser->editMetadataAction()->isEnabled());
        m_browser->editMetadataAction()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<EditMetadataDialog*>() != nullptr, 10000);
        auto* dlg = m_window->findChild<EditMetadataDialog*>();
        QVERIFY(dlg);
        QVERIFY(!dlg->isBulk());
        dlg->reject();
        // The provider (AWS) offers a choice of classes.
        QVERIFY(m_browser->changeClassAction()->isEnabled());
    }

    void aReadOnlyAccountOffersNoChanges()
    {
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        m_browser->view()->selectRow(1);
        QAction* del = m_browser->deleteAction();
        QVERIFY(!del->isEnabled());
        QVERIFY(del->toolTip().contains(QLatin1String("read-only")));
        // Reading stays possible.
        QVERIFY(m_browser->downloadAction()->isEnabled());
    }

    void unfinishedTransfersAreOfferedAtStart()
    {
        // The last session left an upload behind.
        m_window.reset();
        const QString jobs = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                           + QStringLiteral("/transfers/jobs");
        QDir(jobs).removeRecursively();
        {
            TransferJobStore store(jobs);
            TransferJobStore::Record r;
            r.id = QStringLiteral("deadbeef");
            r.kind = Job::TransferKind::Upload;
            r.account = m_h->account.id;
            r.bucket = QStringLiteral("bkt");
            r.title = QStringLiteral("Upload “trip” to s3://bkt/");
            r.items = 3;
            store.save(r);
            QVERIFY(store.waitForWrites(5000));
            QFile plan(store.planPath(r.id));
            QVERIFY(plan.open(QIODevice::WriteOnly));
            plan.write("x\n");
        }
        m_window = std::make_unique<MainWindow>(m_settings.get(), m_theme.get(), m_h->accounts.get(), m_h->secrets.get());
        m_window->show();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<QMessageBox*>() != nullptr, 10000);
        QMessageBox* box = m_window->findChild<QMessageBox*>();
        QVERIFY(box->text().contains(QLatin1String("1 transfer")));
        QVERIFY(box->detailedText().contains(QLatin1String("trip")));
        QAbstractButton* discard = nullptr;
        for (QAbstractButton* b : box->buttons()) {
            if (b->text() == QLatin1String("Discard"))
                discard = b;
        }
        QVERIFY(discard);
        discard->click();
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(jobs + QStringLiteral("/deadbeef.json")), 10000);
        QTRY_VERIFY(!QFile::exists(jobs + QStringLiteral("/deadbeef.plan")));
        QDir(jobs).removeRecursively();
    }

private:
    std::unique_ptr<SettingsController> m_settings;
    std::unique_ptr<ThemeManager> m_theme;
    std::unique_ptr<AccountHarness> m_h;
    FakeBucket m_bucket;
    std::unique_ptr<MainWindow> m_window;
    ObjectBrowser* m_browser = nullptr;
};

QTEST_MAIN(TestMainWindowOperations)
#include "test_main_window_operations.moc"
