#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/BrowseController.h"
#include "models/ObjectSelection.h"
#include "platform/ExternalDrag.h"
#include "support/DragPosition.h"
#include "support/FakeBucket.h"
#include "support/FakeKeychainBackend.h"
#include "ui/ObjectBrowser.h"
#include "ui/PathBar.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTableView>
#include <QUrl>
#include <QtTest>

#include <optional>
#include <tuple>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::DragPosition;
using qbucket::testing::FakeBucket;

namespace {

const QString kKeyId = QStringLiteral("AKIDBROWSER");

// Records what the browser hands to the platform's drag.
class RecordingDrag : public ExternalDrag {
public:
    using ExternalDrag::ExternalDrag;
    Qt::DropAction exec(QObject*, QMimeData* internal, const DragStaging::Selection& selection, Qt::DropActions,
                        Qt::DropAction) override
    {
        mime.reset(internal);
        selections.append(selection);
        return Qt::CopyAction;
    }
    std::unique_ptr<QMimeData> mime;
    QList<DragStaging::Selection> selections;
};
const QString kSecret = QStringLiteral("browser-secret");
using State = ObjectTableModel::State;

} // namespace

class TestObjectBrowser : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_theme.apply(QStringLiteral("Dark"));
    }

    void init()
    {
        QVERIFY(m_dir.isValid());
        m_server = std::make_unique<FakeS3Server>();
        QVERIFY(m_server->listen());
        s3::Credentials c;
        c.accessKeyId = kKeyId;
        c.secretAccessKey = kSecret;
        m_server->setVerifySignatures(c, "test-1");
        m_bucket = FakeBucket();
        m_server->setFallbackHandler([this](const RecordedRequest& req) { return m_bucket.list(req); });

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run)));
        auto backend = std::make_unique<qbucket::testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("Hetzner nbg1");
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
    }

    void cleanup()
    {
        m_sessions.reset();
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void rowsDragOutThroughThePlatform()
    {
        m_bucket.add(QStringLiteral("docs/"), 0);
        m_bucket.add(QStringLiteral("readme.txt"), 7);
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(700, 400);
        browser.show();
        RecordingDrag drag;
        browser.setExternalDrag(&drag, nullptr);
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        browser.view()->selectAll();
        browser.setDownloadsAllowed(true, QString());
        browser.view()->startDragForTesting(Qt::CopyAction | Qt::MoveAction);
        QCOMPARE(drag.selections.size(), 1);
        const DragStaging::Selection& sel = drag.selections.first();
        QCOMPARE(sel.account, m_account);
        QCOMPARE(sel.bucket, QStringLiteral("bkt"));
        QCOMPARE(sel.items.size(), 2);
        QCOMPARE(sel.items.at(0).key, QStringLiteral("docs/"));
        QVERIFY(sel.items.at(0).folder);
        QCOMPARE(sel.items.at(1).key, QStringLiteral("readme.txt"));
        QCOMPARE(sel.items.at(1).size, 7);
        // The in-app formats travel along: drops on folders and buckets work as before.
        QVERIFY(drag.mime);
        QVERIFY(ObjectSelection::fromMimeData(drag.mime.get()).has_value());
        // No staging here: the preparing action stays out of sight.
        QVERIFY(!browser.prepareDragAction()->isVisible());
    }

    void withoutSessionsItShowsAHint()
    {
        ObjectBrowser browser(nullptr, nullptr);
        QVERIFY(!browser.model());
        QVERIFY(message(browser).contains(QLatin1String("Add an account")));
        browser.refresh();   // inert
    }

    void browsesIntoFoldersAndBack()
    {
        m_bucket.add(QStringLiteral("photos/2026/a.jpg"), 100);
        m_bucket.add(QStringLiteral("photos/b.jpg"), 50);
        m_bucket.add(QStringLiteral("readme.txt"), 7);
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(700, 400);
        browser.show();
        QVERIFY(message(browser).contains(QLatin1String("Select a bucket")));

        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QVERIFY(listShown(browser));
        QCOMPARE(browser.model()->rowCount(), 2);
        QVERIFY(browser.statusText().contains(QLatin1String("1 object")));
        QVERIFY(crumbTexts(browser).contains(QStringLiteral("bkt")));

        // Enter on a folder opens it.
        browser.view()->setCurrentIndex(browser.model()->index(0, 0));
        QTest::keyClick(browser.view(), Qt::Key_Return);
        QCOMPARE(browser.navigation()->current().prefix, QStringLiteral("photos/"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QCOMPARE(browser.model()->rowCount(), 2);
        QCOMPARE(crumbTexts(browser).last(), QStringLiteral("photos"));

        // The bucket crumb goes back to the root, through the cache.
        QToolButton* root = crumb(browser, QStringLiteral("bkt"));
        QVERIFY(root);
        root->click();
        QCOMPARE(browser.navigation()->current().prefix, QString());
        QVERIFY(browser.model()->isStale());
        QVERIFY(bannerShown(browser));
        QTRY_VERIFY_WITH_TIMEOUT(!bannerShown(browser), 10000);

        browser.navigation()->back();
        QCOMPARE(browser.navigation()->current().prefix, QStringLiteral("photos/"));
    }

    void localFilesCanBeDroppedOnTheListAndOnFolders()
    {
        m_bucket.add(QStringLiteral("photos/a.jpg"), 1);
        m_bucket.add(QStringLiteral("readme.txt"), 1);
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(700, 400);
        browser.show();
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QSignalSpy dropped(&browser, &ObjectBrowser::filesDropped);
        QWidget* viewport = browser.view()->viewport();

        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("a.txt"))),
                       QUrl::fromLocalFile(m_dir.path())});
        const auto dragTo = [&](const QPoint& pos, QMimeData* mime) {
            QDragEnterEvent enter(DragPosition(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &enter);
            QDragMoveEvent move(DragPosition(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &move);
            const bool accepted = move.isAccepted();
            QDropEvent drop(QPointF(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &drop);
            return accepted;
        };

        // Not allowed (read-only account, no bucket rights): refused.
        QVERIFY(!dragTo(QPoint(5, 5), &files));
        QCOMPARE(dropped.size(), 0);

        browser.setUploadsAllowed(true);
        // On the folder row: into that folder.
        const QRect folderRow = browser.view()->visualRect(browser.model()->index(0, 0));
        QVERIFY(dragTo(folderRow.center(), &files));
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(dropped.last().at(0).toStringList().size(), 2);
        QCOMPARE(dropped.last().at(1).toString(), QStringLiteral("photos/"));
        // On a file row or empty space: into the current folder.
        const QRect fileRow = browser.view()->visualRect(browser.model()->index(1, 0));
        QVERIFY(dragTo(fileRow.center(), &files));
        QCOMPARE(dropped.last().at(1).toString(), QString());

        // A web link is not a local file: refused as a whole.
        QMimeData web;
        web.setUrls({QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("a.txt"))), QUrl(QStringLiteral("https://example.com/x"))});
        QVERIFY(!dragTo(fileRow.center(), &web));
        QCOMPARE(dropped.size(), 2);
        QVERIFY(ObjectBrowser::localPaths(&web).isEmpty());
    }

    // Ctrl+C / X / V: objects on the clipboard, pasted into the current folder.
    void copyCutAndPasteObjects()
    {
        m_bucket.add(QStringLiteral("docs/a.txt"));
        m_bucket.add(QStringLiteral("docs/sub/x"));
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.show();
        browser.navigation()->navigate({m_account, QStringLiteral("bkt"), QStringLiteral("docs/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QList<std::pair<ObjectSelection, QString>> pasted;
        connect(&browser, &ObjectBrowser::pasteRequested, this,
                [&](const ObjectSelection& sel, const QString& prefix) { pasted.append({sel, prefix}); });
        QTableView* view = browser.view();
        view->setFocus();
        view->selectAll();
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
        std::optional<ObjectSelection> sel = ObjectSelection::fromMimeData(QGuiApplication::clipboard()->mimeData());
        QVERIFY(sel);
        QCOMPARE(sel->intent, ObjectSelection::Intent::Copy);
        QCOMPARE(sel->base, QStringLiteral("docs/"));
        QCOMPARE(sel->items.size(), 2);
        QVERIFY(sel->items.first().folder);   // docs/sub/
        QVERIFY(QGuiApplication::clipboard()->text().contains(QLatin1String("s3://bkt/docs/a.txt")));

        // Nothing may be written here (yet): Paste is off and says why.
        QVERIFY(!browser.pasteAction()->isEnabled());
        browser.setUploadsAllowed(true);
        browser.setMutationsAllowed(true, QString());   // re-evaluates the actions
        QVERIFY(browser.pasteAction()->isEnabled());
        QTest::keyClick(view, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.first().second, QStringLiteral("docs/"));

        QTest::keyClick(view, Qt::Key_X, Qt::ControlModifier);
        sel = ObjectSelection::fromMimeData(QGuiApplication::clipboard()->mimeData());
        QVERIFY(sel);
        QCOMPARE(sel->intent, ObjectSelection::Intent::Cut);

        // Another account's objects are not pasted here (between accounts: later).
        ObjectSelection other = *sel;
        other.account = QUuid::createUuid();
        QGuiApplication::clipboard()->setMimeData(other.toMimeData());
        QTRY_VERIFY(!browser.pasteAction()->isEnabled());
        QVERIFY(browser.pasteAction()->toolTip().contains(QLatin1String("between accounts")));
        // Plain text is nothing to paste.
        QGuiApplication::clipboard()->setText(QStringLiteral("docs/a.txt"));
        QTRY_VERIFY(!browser.pasteAction()->isEnabled());
    }

    // Objects dragged within the list: onto a folder row, copy or (Shift) move.
    void objectsCanBeDraggedOntoFolders()
    {
        m_bucket.add(QStringLiteral("docs/a.txt"), 1);
        m_bucket.add(QStringLiteral("docs/sub/x"), 1);
        m_bucket.add(QStringLiteral("docs/zz.txt"), 1);
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(700, 400);
        browser.show();
        browser.navigation()->navigate({m_account, QStringLiteral("bkt"), QStringLiteral("docs/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        browser.setUploadsAllowed(true);
        QList<std::tuple<ObjectSelection, QString, bool>> dropped;
        connect(&browser, &ObjectBrowser::objectsDropped, this,
                [&](const ObjectSelection& sel, const QString& prefix, bool move) { dropped.append({sel, prefix, move}); });
        QWidget* viewport = browser.view()->viewport();
        const auto dragTo = [&](const QPoint& pos, const QMimeData* mime, Qt::KeyboardModifiers mods) {
            QDragEnterEvent enter(DragPosition(pos), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(viewport, &enter);
            QDragMoveEvent move(DragPosition(pos), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(viewport, &move);
            const bool accepted = move.isAccepted();
            QDropEvent drop(QPointF(pos), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(viewport, &drop);
            return accepted;
        };
        // What dragging a.txt and zz.txt carries.
        std::unique_ptr<QMimeData> mime(browser.model()->mimeData({browser.model()->index(1, 0), browser.model()->index(2, 0)}));
        const std::optional<ObjectSelection> sel = ObjectSelection::fromMimeData(mime.get());
        QVERIFY(sel);
        QCOMPARE(sel->items.size(), 2);
        QCOMPARE(sel->base, QStringLiteral("docs/"));

        const QRect folderRow = browser.view()->visualRect(browser.model()->index(0, 0));   // docs/sub/
        QVERIFY(dragTo(folderRow.center(), mime.get(), Qt::NoModifier));
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(std::get<1>(dropped.last()), QStringLiteral("docs/sub/"));
        QVERIFY(!std::get<2>(dropped.last()));
        QVERIFY(dragTo(folderRow.center(), mime.get(), Qt::ShiftModifier));
        QVERIFY(std::get<2>(dropped.last()));   // Shift: move

        // Back where they are (a file row: the current folder) — a slip, ignored.
        const QRect fileRow = browser.view()->visualRect(browser.model()->index(1, 0));
        QVERIFY(!dragTo(fileRow.center(), mime.get(), Qt::NoModifier));
        // A folder onto itself, and another account's objects: ignored.
        std::unique_ptr<QMimeData> folder(browser.model()->mimeData({browser.model()->index(0, 0)}));
        QVERIFY(!dragTo(folderRow.center(), folder.get(), Qt::NoModifier));
        ObjectSelection foreign = *sel;
        foreign.account = QUuid::createUuid();
        std::unique_ptr<QMimeData> foreignMime(foreign.toMimeData());
        QVERIFY(!dragTo(folderRow.center(), foreignMime.get(), Qt::NoModifier));
        QCOMPARE(dropped.size(), 2);
    }

    void theVersionsViewListsVersionsAndChangesNothing()
    {
        FakeObjectStore store(m_server.get());   // replaces the FakeBucket fallback
        store.createBucket(QStringLiteral("bkt"));
        store.setVersioning(QStringLiteral("bkt"), QStringLiteral("Enabled"));
        store.putObject(QStringLiteral("bkt"), QStringLiteral("a.txt"), "1");
        store.putObject(QStringLiteral("bkt"), QStringLiteral("a.txt"), "22");
        store.putObject(QStringLiteral("bkt"), QStringLiteral("b.txt"), "3");
        store.deleteObject(QStringLiteral("bkt"), QStringLiteral("b.txt"));
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(800, 400);
        browser.show();
        browser.setMutationsAllowed(true, QString());
        browser.setDownloadsAllowed(true, QString());
        QVERIFY(browser.view()->isColumnHidden(ObjectTableModel::VersionColumn));

        browser.setMode(ObjectTableModel::Mode::Versions);
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QCOMPARE(browser.model()->rowCount(), 4);
        QVERIFY(!browser.view()->isColumnHidden(ObjectTableModel::VersionColumn));
        QVERIFY(browser.statusText().contains(QLatin1String("4 version")));   // "(s)" without the plurals file

        browser.view()->selectRow(1);   // a.txt's earlier version
        QVERIFY(browser.copyKeyAction()->isEnabled());
        QVERIFY(browser.copyUriAction()->isEnabled());
        for (QAction* a : {browser.copyAction(), browser.cutAction(), browser.renameAction(),
                           browser.editMetadataAction()}) {
            QVERIFY2(!a->isEnabled(), qPrintable(a->text()));
            QVERIFY(a->toolTip().contains(QLatin1String("versions view")));
        }
        // Its own actions — writes only once the gate allows them.
        QVERIFY(browser.downloadAction()->isEnabled());
        QVERIFY(browser.presignAction()->isEnabled());
        QVERIFY(!browser.restoreVersionAction()->isEnabled());
        QVERIFY(!browser.deleteAction()->isEnabled());
        browser.setVersionActionsAllowed(true, QString());
        QVERIFY(browser.restoreVersionAction()->isEnabled());
        QVERIFY(browser.deleteAction()->isEnabled());
        QVERIFY(browser.deleteAction()->text().contains(QLatin1String("Permanently")));
        QVERIFY(!browser.removeMarkerAction()->isEnabled());
        QCOMPARE(browser.selectedVersions().size(), 1);
        QVERIFY(!browser.selectedVersions().first().latest);

        browser.view()->clearSelection();
        browser.view()->selectRow(0);   // a.txt's current version: nothing to restore
        QVERIFY(!browser.restoreVersionAction()->isEnabled());
        const int marker = browser.model()->rowOfKey(QStringLiteral("b.txt"));
        browser.view()->clearSelection();
        browser.view()->selectRow(marker);   // b.txt's delete marker
        QVERIFY(browser.removeMarkerAction()->isEnabled());
        QVERIFY(!browser.downloadAction()->isEnabled());   // a marker has no content
        QVERIFY(browser.deleteAllVersionsAction()->isEnabled());

        browser.setMode(ObjectTableModel::Mode::Folders);
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QCOMPARE(browser.model()->rowCount(), 1);   // b.txt is deleted
        QVERIFY(browser.view()->isColumnHidden(ObjectTableModel::VersionColumn));
        browser.view()->selectRow(0);
        QVERIFY(browser.deleteAction()->isEnabled());
        QCOMPARE(browser.deleteAction()->text(), QStringLiteral("&Delete…"));
        QVERIFY(browser.restoreDeletedAction()->isEnabled());
        QVERIFY(!browser.removeMarkerAction()->isEnabled());
    }

    void anEmptyFolderAndAnErrorSayWhatTheyAre()
    {
        m_bucket.add(QStringLiteral("empty/"));
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.show();
        browser.navigation()->navigate({m_account, QStringLiteral("bkt"), QStringLiteral("empty/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QCOMPARE(message(browser), QStringLiteral("This folder is empty."));
        QVERIFY(!retryButton(browser)->isVisible());

        int calls = 0;
        m_server->setFallbackHandler([&](const RecordedRequest& req) {
            return ++calls == 1 ? FakeResponse::s3Error(403, "AccessDenied", "nope") : m_bucket.list(req);
        });
        browser.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Error, 10000);
        QVERIFY(!message(browser).isEmpty());
        QVERIFY(message(browser) != QStringLiteral("This folder is empty."));
        QVERIFY(retryButton(browser)->isVisible());
        retryButton(browser)->click();
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QCOMPARE(message(browser), QStringLiteral("This folder is empty."));
    }

    // An empty folder shows a message, not the list — the first upload
    // happens there: files and objects drop in, the context menu offers
    // the location's actions (upload, new folder) and Paste.
    void anEmptyFolderTakesDropsAndHasAContextMenu()
    {
        m_bucket.add(QStringLiteral("empty/"));
        m_bucket.add(QStringLiteral("readme.txt"), 1);
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(700, 400);
        browser.show();
        QAction uploadHere(QStringLiteral("Upload Here"));
        browser.setLocationActions({&uploadHere});
        browser.navigation()->navigate({m_account, QStringLiteral("bkt"), QStringLiteral("empty/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QCOMPARE(message(browser), QStringLiteral("This folder is empty."));
        QWidget* page = browser.findChild<QLabel*>(QStringLiteral("emptyState"))->parentWidget();
        QSignalSpy filesDropped(&browser, &ObjectBrowser::filesDropped);
        QSignalSpy objectsDropped(&browser, &ObjectBrowser::objectsDropped);
        const auto dragTo = [&](const QMimeData* mime) {
            const QPoint pos = page->rect().center();
            QDragEnterEvent enter(DragPosition(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(page, &enter);
            QDragMoveEvent move(DragPosition(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(page, &move);
            const bool accepted = move.isAccepted();
            QDropEvent drop(QPointF(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(page, &drop);
            return accepted;
        };
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("a.txt")))});

        // Not allowed (read-only account, no bucket rights): refused.
        QVERIFY(page->acceptDrops());
        QVERIFY(!dragTo(&files));
        QCOMPARE(filesDropped.size(), 0);

        browser.setUploadsAllowed(true);
        QVERIFY(dragTo(&files));
        QCOMPARE(filesDropped.size(), 1);
        QCOMPARE(filesDropped.last().at(1).toString(), QStringLiteral("empty/"));
        // Objects of this bucket from elsewhere land here too.
        ObjectSelection sel;
        sel.account = m_account;
        sel.bucket = QStringLiteral("bkt");
        sel.items.append({QStringLiteral("readme.txt"), false});
        std::unique_ptr<QMimeData> objects(sel.toMimeData());
        QVERIFY(dragTo(objects.get()));
        QCOMPARE(objectsDropped.size(), 1);
        QCOMPARE(objectsDropped.last().at(1).toString(), QStringLiteral("empty/"));

        QStringList menu = contextMenuOf(page);
        QVERIFY2(menu.contains(QStringLiteral("Upload Here")), qPrintable(menu.join(u'|')));
        QVERIFY(menu.contains(browser.pasteAction()->text()));
        QVERIFY(!menu.contains(browser.deleteAction()->text()));   // nothing selected here

        // The versions view takes nothing, and offers no upload.
        browser.setMode(ObjectTableModel::Mode::Versions);
        QVERIFY(!dragTo(&files));
        QVERIFY(!contextMenuOf(page).contains(QStringLiteral("Upload Here")));
        browser.setMode(ObjectTableModel::Mode::Folders);
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);

        // An error is no folder to upload into.
        m_server->setFallbackHandler([](const RecordedRequest&) { return FakeResponse::s3Error(403, "AccessDenied", "nope"); });
        browser.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Error, 10000);
        QVERIFY(!dragTo(&files));
        QCOMPARE(filesDropped.size(), 1);
        QVERIFY(contextMenuOf(page).isEmpty());
    }

    // The list's context menu offers the location's actions as well.
    void theListsContextMenuOffersTheLocationActions()
    {
        m_bucket.add(QStringLiteral("readme.txt"), 1);
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(700, 400);
        browser.show();
        QAction uploadHere(QStringLiteral("Upload Here"));
        browser.setLocationActions({&uploadHere});
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QVERIFY(listShown(browser));
        QVERIFY(contextMenuOf(browser.view()).contains(QStringLiteral("Upload Here")));
    }

    void scrollingNearTheEndLoadsTheNextPage()
    {
        for (int i = 0; i < 2500; ++i)
            m_bucket.add(QStringLiteral("k%1").arg(i, 5, 10, QLatin1Char('0')));
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(600, 300);
        browser.show();
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->rowCount(), 1000, 10000);
        QCOMPARE(browser.model()->state(), State::Ready);
        QVERIFY(browser.statusText().contains(QLatin1String("so far")));

        QScrollBar* sb = browser.view()->verticalScrollBar();
        QTRY_VERIFY(sb->maximum() > 0);
        sb->setValue(sb->maximum() - sb->pageStep());   // not the very end yet
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->rowCount(), 2000, 10000);
    }

    void theCurrentRowSurvivesTheRefreshOfACachedListing()
    {
        m_bucket.add(QStringLiteral("a/x"));
        m_bucket.add(QStringLiteral("b.txt"));
        m_bucket.add(QStringLiteral("c.txt"));
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.show();
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        browser.navigation()->navigate({m_account, QStringLiteral("bkt"), QStringLiteral("a/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        browser.navigation()->back();
        QVERIFY(browser.model()->isStale());
        browser.view()->setCurrentIndex(browser.model()->index(2, 0));   // c.txt
        QTRY_VERIFY_WITH_TIMEOUT(!browser.model()->isStale(), 10000);
        QCOMPARE(browser.view()->currentIndex().data(ObjectTableModel::KeyRole).toString(), QStringLiteral("c.txt"));
    }

    void aFilterThatHidesEverythingOffersMore()
    {
        for (int i = 0; i < 1500; ++i)
            m_bucket.add(QStringLiteral("k%1").arg(i, 5, 10, QLatin1Char('0')));
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.show();
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->rowCount(), 1000, 10000);
        browser.setFilterText(QStringLiteral("k0149"));
        QVERIFY(message(browser).contains(QLatin1String("k0149")));
        QVERIFY(retryButton(browser)->isVisible());
        retryButton(browser)->click();   // "Load More"
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->rowCount(), 10, 10000);
        QVERIFY(listShown(browser));
    }

    void typedPathsNavigate()
    {
        m_bucket.add(QStringLiteral("deep/er/x"));
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.show();
        browser.openBucket(m_account, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        browser.editPath();
        auto* edit = browser.pathBar()->findChild<QLineEdit*>(QStringLiteral("pathEdit"));
        QVERIFY(edit);
        QCOMPARE(edit->text(), QStringLiteral("bkt/"));
        edit->setText(QStringLiteral("s3://bkt/deep/er"));
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(browser.navigation()->current().prefix, QStringLiteral("deep/er/"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->rowCount(), 1, 10000);
    }

    void keysOnTheList()
    {
        m_bucket.add(QStringLiteral("docs/a b.txt"));
        m_bucket.add(QStringLiteral("docs/c.txt"));
        m_bucket.add(QStringLiteral("docs/sub/x"));
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.show();
        browser.navigation()->navigate({m_account, QStringLiteral("bkt"), QStringLiteral("docs/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QTableView* view = browser.view();
        view->setFocus();
        view->selectAll();
        QVERIFY(browser.copyKeyAction()->isEnabled());
        QVERIFY(!browser.openAction()->isEnabled());   // three rows
        QVERIFY(!browser.renameAction()->isEnabled());
        QVERIFY(!browser.deleteAction()->isEnabled());

        // Ctrl+Shift+C: the keys; Ctrl+Alt+C: the URIs (Ctrl+C copies the objects).
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("docs/sub/\ndocs/a b.txt\ndocs/c.txt"));
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(QGuiApplication::clipboard()->text().startsWith(QLatin1String("s3://bkt/docs/sub/\n")));

        // The folder alone (explicitly: a synthetic Ctrl+Alt key click can leave
        // modifiers behind that would make the click a toggle).
        view->selectionModel()->setCurrentIndex(browser.model()->index(0, 0),
                                                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QVERIFY(browser.openAction()->isEnabled());
        browser.openAction()->trigger();
        QCOMPARE(browser.navigation()->current().prefix, QStringLiteral("docs/sub/"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QVERIFY(view->hasFocus());   // back on the list after "Loading…"
        QTest::keyClick(view, Qt::Key_Backspace);
        QCOMPARE(browser.navigation()->current().prefix, QStringLiteral("docs/"));
    }

    // QBUCKET_SCREENSHOT_DIR set: saves browser-dark.png (devlog).
    void screenshot()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        m_bucket.add(QStringLiteral("vm/101/2026-10/a"));
        m_bucket.add(QStringLiteral("vm/101/disk-0.img"), Q_INT64_C(42949672960));
        m_bucket.add(QStringLiteral("vm/101/index.json"), 12288);
        m_bucket.add(QStringLiteral("vm/101/notes.md"), 2100, "STANDARD_IA");
        m_bucket.add(QStringLiteral("vm/101/2025-archive.tar.zst"), Q_INT64_C(9126805504), "GLACIER");
        m_bucket.add(QStringLiteral("vm/101/screenshot.png"), 482000);
        m_bucket.sort();
        ObjectBrowser browser(m_accounts.get(), m_sessions.get());
        browser.resize(760, 300);
        browser.show();
        browser.navigation()->navigate({m_account, QStringLiteral("pbs-backup"), QStringLiteral("vm/101/")});
        QTRY_COMPARE_WITH_TIMEOUT(browser.model()->state(), State::Complete, 10000);
        QTest::qWait(150);
        QDir().mkpath(dir);
        QVERIFY(browser.grab().save(QDir(dir).filePath(QStringLiteral("browser-dark.png"))));
    }

private:
    static QString message(const ObjectBrowser& b)
    {
        const auto* label = b.findChild<QLabel*>(QStringLiteral("emptyState"));
        return label && label->isVisibleTo(const_cast<ObjectBrowser*>(&b)) ? label->text() : QString();
    }
    // The texts of the context menu `w` opens (empty: none opened).
    static QStringList contextMenuOf(QWidget* w)
    {
        QStringList texts;
        QTimer closer;
        closer.setInterval(10);
        QObject::connect(&closer, &QTimer::timeout, &closer, [&texts] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (const QAction* a : menu->actions()) {
                if (!a->isSeparator())
                    texts.append(a->text());
            }
            menu->close();
        });
        closer.start();
        emit w->customContextMenuRequested(QPoint(5, 5));
        return texts;
    }
    static bool listShown(const ObjectBrowser& b)
    {
        return b.findChild<QStackedWidget*>()->currentWidget() == b.view();
    }
    static bool bannerShown(const ObjectBrowser& b)
    {
        for (const QLabel* l : b.findChildren<QLabel*>()) {
            if (l->objectName().startsWith(QLatin1String("banner")))
                return !l->isHidden();
        }
        return false;
    }
    static QPushButton* retryButton(const ObjectBrowser& b)
    {
        return b.findChild<QPushButton*>();
    }
    static QStringList crumbTexts(const ObjectBrowser& b)
    {
        QStringList out;
        for (const QToolButton* t : b.pathBar()->findChildren<QToolButton*>()) {
            if (!t->isHidden())
                out.append(t->text());
        }
        return out;
    }
    static QToolButton* crumb(const ObjectBrowser& b, const QString& text)
    {
        for (QToolButton* t : b.pathBar()->findChildren<QToolButton*>()) {
            if (!t->isHidden() && t->text() == text)
                return t;
        }
        return nullptr;
    }

    ThemeManager m_theme;
    QTemporaryDir m_dir;
    int m_run = 0;
    FakeBucket m_bucket;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
    QUuid m_account;
};

QTEST_MAIN(TestObjectBrowser)
#include "test_object_browser.moc"
