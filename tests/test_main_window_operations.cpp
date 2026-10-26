#include "accounts/LocationStore.h"
#include "controllers/BrowseController.h"
#include "controllers/SettingsController.h"
#include "controllers/TransferJobStore.h"
#include "models/AccountTreeModel.h"
#include "support/AccountHarness.h"
#include "support/DragPosition.h"
#include "models/ObjectSelection.h"
#include "support/FakeBucket.h"
#include "ui/AccountSidebar.h"
#include "ui/CreateBucketDialog.h"
#include "ui/MainWindow.h"
#include "ui/ObjectBrowser.h"
#include "ui/EditMetadataDialog.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QClipboard>
#include <QGuiApplication>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;
using qbucket::testing::FakeBucket;

namespace {

// Counts how often a widget is shown; remembers the job label beside it.
class ShowCounter : public QObject {
public:
    explicit ShowCounter(QWidget* w)
        : m_widget(w)
    {
        w->installEventFilter(this);
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_widget && event->type() == QEvent::Show) {
            ++count;
            // The job label sits left of the bar in the status bar.
            for (const QLabel* l : m_widget->parentWidget()->findChildren<QLabel*>()) {
                if (l->isVisibleTo(m_widget->parentWidget()) && l->text().contains(QLatin1String("Delete bucket")))
                    labelText = l->text();
            }
        }
        return false;
    }
    int count = 0;
    QString labelText;

private:
    QWidget* m_widget;
};

} // namespace

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

    void bookmarkTheFolderAndComeBack()
    {
        LocationStore* store = m_window->locations();
        QVERIFY(store);
        for (const Bookmark& b : store->bookmarks())
            store->removeBookmark(b.id);
        const Location doomed{m_h->account.id, QStringLiteral("bkt"), QStringLiteral("doomed/")};
        m_browser->navigation()->navigate(doomed);
        QTRY_VERIFY(m_window->addBookmarkAction()->isEnabled());
        QCOMPARE(m_window->addBookmarkAction()->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_B));
        m_window->addBookmarkAction()->trigger();
        QTRY_VERIFY(m_window->findChild<QInputDialog*>() != nullptr);
        auto* ask = m_window->findChild<QInputDialog*>();
        QVERIFY(ask);
        QCOMPARE(ask->textValue(), QStringLiteral("doomed"));
        ask->setTextValue(QStringLiteral("Doomed"));
        ask->accept();
        QCOMPARE(store->bookmarks().size(), 1);
        QCOMPARE(store->bookmarks().first().location, doomed);
        QVERIFY(store->recent().contains(doomed));

        m_browser->navigation()->navigate({m_h->account.id, QStringLiteral("bkt"), {}});
        emit m_window->bookmarksMenu()->aboutToShow();
        QAction* entry = nullptr;
        for (QAction* a : m_window->bookmarksMenu()->actions()) {
            if (a->text() == QLatin1String("Doomed"))
                entry = a;
        }
        QVERIFY(entry);
        if (!entry)
            return;
        entry->trigger();
        QCOMPARE(m_browser->navigation()->current(), doomed);

        // The account goes, its places too.
        QVERIFY(m_h->accounts->remove(m_h->account.id));
        QVERIFY(store->bookmarks().isEmpty());
        QVERIFY(!store->recent().contains(doomed));
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

    // Deleting a bucket with what is in it: the status bar shows that it
    // runs (bar + step), the end is a message that can't be missed.
    void deletingABucketShowsProgressAndTheEnd()
    {
        FakeObjectStore store(m_h->server.get());   // replaces the FakeBucket fallback
        store.createBucket(QStringLiteral("gone"));
        store.putObject(QStringLiteral("gone"), QStringLiteral("a.txt"), "1");
        store.putObject(QStringLiteral("gone"), QStringLiteral("dir/b.txt"), "2");
        // Listed in the sidebar and open in the browser.
        auto* sidebar = m_window->findChild<AccountSidebar*>();
        AccountTreeModel* tree = sidebar->model();
        tree->refresh(tree->accountIndex(m_h->account.id));
        QTRY_VERIFY_WITH_TIMEOUT(tree->bucketIndex(m_h->account.id, QStringLiteral("gone")).isValid(), 10000);
        m_browser->openBucket(m_h->account.id, QStringLiteral("gone"));
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
        QCOMPARE(m_browser->model()->rowCount(), 2);   // a.txt, dir/
        auto* bar = m_window->findChild<QProgressBar*>(QStringLiteral("jobProgress"));
        QVERIFY(bar);
        QVERIFY(!bar->isVisible());
        // Slim: neither the bar nor Cancel makes the status bar taller than its text.
        m_theme->apply(QStringLiteral("Dark"));
        bar->ensurePolished();
        QCOMPARE(bar->maximumHeight(), 8);   // 6 px and the border
        QToolButton* cancel = nullptr;
        for (QToolButton* b : m_window->statusBar()->findChildren<QToolButton*>()) {
            if (b->text() == QLatin1String("Cancel"))
                cancel = b;
        }
        QVERIFY(cancel);
        cancel->ensurePolished();
        QVERIFY2(cancel->sizeHint().height() <= QFontMetrics(cancel->font()).height() + 6,
                 qPrintable(QString::number(cancel->sizeHint().height())));
        ShowCounter shown(bar);

        deleteBucketWithEverything(QStringLiteral("gone"));
        if (QTest::currentTestFailed())
            return;

        QTRY_VERIFY_WITH_TIMEOUT(!store.hasBucket(QStringLiteral("gone")), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<QMessageBox*>(QStringLiteral("bucketDeletedBox")) != nullptr, 20000);
        auto* done = m_window->findChild<QMessageBox*>(QStringLiteral("bucketDeletedBox"));
        QVERIFY(done->text().contains(QLatin1String("gone")));
        QVERIFY(shown.count > 0);          // the bar showed while it ran …
        QVERIFY(!shown.labelText.isEmpty());
        QTRY_VERIFY(!bar->isVisible());    // … and goes when it's done
        // Gone from the sidebar; the browser left it.
        QTRY_VERIFY_WITH_TIMEOUT(!tree->bucketIndex(m_h->account.id, QStringLiteral("gone")).isValid(), 10000);
        QVERIFY(!m_browser->navigation()->current().hasBucket());
        done->close();
        m_window.reset();   // before the store: the window's sessions still point at it
    }

    // Emptied only in part — a version under retention keeps the bucket —
    // while the browser shows it: listed afresh, not the objects that went.
    void aBucketThatStaysIsListedAfresh()
    {
        FakeObjectStore store(m_h->server.get());
        store.createBucket(QStringLiteral("held"));
        store.setObjectLockEnabled(QStringLiteral("held"));
        store.putObject(QStringLiteral("held"), QStringLiteral("free.txt"), "1");
        const QString v = store.putObject(QStringLiteral("held"), QStringLiteral("kept.txt"), "2");
        store.setRetention(QStringLiteral("held"), QStringLiteral("kept.txt"), v, s3::ObjectLockMode::Governance,
                           QDateTime::currentDateTimeUtc().addDays(1));
        m_browser->openBucket(m_h->account.id, QStringLiteral("held"));
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
        QCOMPARE(m_browser->model()->rowCount(), 2);

        deleteBucketWithEverything(QStringLiteral("held"));
        if (QTest::currentTestFailed())
            return;

        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->rowCount(), 1, 20000);
        QCOMPARE(m_browser->model()->index(0, 0).data().toString(), QStringLiteral("kept.txt"));
        QVERIFY(store.hasBucket(QStringLiteral("held")));
        QCOMPARE(m_browser->navigation()->current().bucket, QStringLiteral("held"));   // still there: stays open
        m_window.reset();   // before the store
    }

    // A new bucket, made here, opened from the sidebar, a file dropped onto
    // its empty page: the file shows once uploaded.
    void aFileDroppedIntoANewBucketShowsUp()
    {
        FakeObjectStore store(m_h->server.get());
        newBucketDropFlow(m_h->account.id, QStringLiteral("fresh"), 10000);
        m_window.reset();   // before the store
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
    // Creates `bucket` through the sidebar's New Bucket, opens it by making
    // it current in the sidebar, drops a local file onto its empty page,
    // confirms the upload and expects the file in the list.
    void newBucketDropFlow(const QUuid& account, const QString& bucket, int timeoutMs)
    {
        auto* sidebar = m_window->findChild<AccountSidebar*>();
        AccountTreeModel* tree = sidebar->model();
        tree->refresh(tree->accountIndex(account));
        QTRY_VERIFY_WITH_TIMEOUT(tree->rowCount(tree->accountIndex(account)) > 0, timeoutMs);

        emit sidebar->createBucketRequested(account);
        QTRY_VERIFY(m_window->findChild<CreateBucketDialog*>() != nullptr);
        auto* create = m_window->findChild<CreateBucketDialog*>();
        create->nameEdit()->setText(bucket);
        QTRY_VERIFY(create->createButton()->isEnabled());
        create->createButton()->click();
        // Confirmed by the dialog; a preview only if the job asks for one.
        QTRY_VERIFY_WITH_TIMEOUT(tree->bucketIndex(account, bucket).isValid()
                                     || m_window->findChild<OperationPreviewDialog*>() != nullptr,
                                 timeoutMs);
        if (auto* p = m_window->findChild<OperationPreviewDialog*>()) {
            if (p->productionCheck())
                p->productionCheck()->setChecked(true);
            if (p->typedConfirmation())
                p->typedConfirmation()->setText(p->expectedTyping());
            QTRY_VERIFY(p->confirmButton()->isEnabled());
            p->confirmButton()->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(tree->bucketIndex(account, bucket).isValid(), timeoutMs);
        for (QMessageBox* b : m_window->findChildren<QMessageBox*>())
            b->close();

        sidebar->view()->setCurrentIndex(tree->bucketIndex(account, bucket));   // a click on it
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->navigation()->current().bucket, bucket, timeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, timeoutMs);
        auto* stack = m_browser->findChild<QStackedWidget*>();
        QVERIFY(stack->currentWidget() != m_browser->view());   // "This folder is empty."

        const QString file = m_h->path(QStringLiteral("hello.txt"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("hello");
        }
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(file)});
        QWidget* page = stack->currentWidget();
        const QPoint pos = page->rect().center();
        QDragEnterEvent enter(qbucket::testing::DragPosition(pos), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(page, &enter);
        QDragMoveEvent move(qbucket::testing::DragPosition(pos), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(page, &move);
        QVERIFY(move.isAccepted());
        QDropEvent drop(QPointF(pos), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(page, &drop);

        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<OperationPreviewDialog*>() != nullptr, timeoutMs);
        auto* dlg = m_window->findChild<OperationPreviewDialog*>();
        QTRY_VERIFY(dlg->isVisible());
        dlg->productionCheck()->setChecked(true);
        QTRY_VERIFY(dlg->confirmButton()->isEnabled());
        dlg->confirmButton()->click();

        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->rowCount(), 1, timeoutMs);
        QCOMPARE(m_browser->model()->index(0, 0).data().toString(), QStringLiteral("hello.txt"));
        QTRY_VERIFY(stack->currentWidget() == m_browser->view());
        QCOMPARE(m_browser->navigation()->current().bucket, bucket);
    }

    // The sidebar's Delete Bucket with "everything in it", through the
    // question and the preview (typed name, production), confirmed.
    void deleteBucketWithEverything(const QString& bucket)
    {
        emit m_window->findChild<AccountSidebar*>()->deleteBucketRequested(m_h->account.id, bucket);
        QTRY_VERIFY(m_window->findChild<QMessageBox*>() != nullptr);
        auto* ask = m_window->findChild<QMessageBox*>();
        QVERIFY(ask && ask->checkBox());
        ask->checkBox()->setChecked(true);
        QAbstractButton* go = nullptr;
        for (QAbstractButton* b : ask->buttons()) {
            if (ask->buttonRole(b) == QMessageBox::AcceptRole)
                go = b;
        }
        QVERIFY(go);
        go->click();

        QTRY_VERIFY_WITH_TIMEOUT(m_window->findChild<OperationPreviewDialog*>() != nullptr, 10000);
        auto* dlg = m_window->findChild<OperationPreviewDialog*>();
        QVERIFY(dlg);
        QTRY_VERIFY(dlg->isVisible());
        dlg->productionCheck()->setChecked(true);
        dlg->typedConfirmation()->setText(dlg->expectedTyping());
        QTRY_VERIFY(dlg->confirmButton()->isEnabled());
        dlg->confirmButton()->click();
    }

    std::unique_ptr<SettingsController> m_settings;
    std::unique_ptr<ThemeManager> m_theme;
    std::unique_ptr<AccountHarness> m_h;
    FakeBucket m_bucket;
    std::unique_ptr<MainWindow> m_window;
    ObjectBrowser* m_browser = nullptr;
};

QTEST_MAIN(TestMainWindowOperations)
#include "test_main_window_operations.moc"
