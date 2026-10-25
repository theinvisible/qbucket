#include "accounts/AccountStore.h"
#include "accounts/LocationStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/AccountTreeModel.h"
#include "support/DragPosition.h"
#include "support/FakeKeychainBackend.h"
#include "models/ObjectSelection.h"
#include "ui/AccountSidebar.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QApplication>
#include <QDir>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QToolButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QMimeData>
#include <QUrl>
#include <QtTest>

#include <tuple>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::DragPosition;

namespace {

const QString kKeyId = QStringLiteral("AKIDSIDEBAR");
const QString kSecret = QStringLiteral("sidebar-secret");
const QByteArray kBuckets = "<ListAllMyBucketsResult><Buckets>"
                            "<Bucket><Name>media</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                            "</Buckets></ListAllMyBucketsResult>";

} // namespace

class TestAccountSidebar : public QObject {
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
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(kBuckets));

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run)));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        m_keychain = backend.get();
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        m_sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
    }

    void cleanup()
    {
        m_sessions.reset();
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void withoutStoresItIsInert()
    {
        AccountSidebar sidebar(nullptr, nullptr, nullptr);
        QVERIFY(!sidebar.model());
        sidebar.refreshCurrent();   // no crash
        const auto* empty = sidebar.findChild<QLabel*>(QStringLiteral("emptyState"));
        QVERIFY(empty);
        QVERIFY(!empty->isHidden());
    }

    void emptyStateFollowsTheAccounts()
    {
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        const auto* empty = sidebar.findChild<QLabel*>(QStringLiteral("emptyState"));
        QVERIFY(!empty->isHidden());
        QVERIFY(sidebar.view()->isHidden());
        addAccount(true);
        QVERIFY(empty->isHidden());
        QVERIFY(!sidebar.view()->isHidden());
    }

    void selectingABucketReportsIt()
    {
        const Account a = addAccount(true);
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.resize(260, 400);
        sidebar.show();
        QSignalSpy selected(&sidebar, &AccountSidebar::bucketSelected);

        sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")).isValid(), 10000);
        sidebar.view()->setCurrentIndex(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().at(0).toUuid(), a.id);
        QCOMPARE(selected.first().at(1).toString(), QStringLiteral("media"));
        sidebar.view()->viewport()->repaint();   // the delegate paints from roles only
    }

    void filesDroppedOnABucketUploadThere()
    {
        const Account a = addAccount(true);
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.resize(260, 400);
        sidebar.show();
        sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")).isValid(), 10000);
        QSignalSpy dropped(&sidebar, &AccountSidebar::filesDroppedOnBucket);
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(QDir::tempPath())});
        const auto dropAt = [&](const QModelIndex& index) {
            const QPoint pos = sidebar.view()->visualRect(index).center();
            QDragEnterEvent enter(DragPosition(pos), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(sidebar.view()->viewport(), &enter);
            QDropEvent drop(QPointF(pos), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(sidebar.view()->viewport(), &drop);
        };
        dropAt(sidebar.model()->accountIndex(a.id));   // an account row: no
        QCOMPARE(dropped.size(), 0);
        dropAt(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")));   // read-only: no
        QCOMPARE(dropped.size(), 0);

        Account writable = a;
        writable.readOnly = false;
        QVERIFY(m_accounts->save(writable));
        sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        QTRY_VERIFY_WITH_TIMEOUT(!sidebar.model()->bucketIndex(a.id, QStringLiteral("media"))
                                      .data(AccountTreeModel::ReadOnlyRole).toBool(), 10000);
        dropAt(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")));
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(dropped.first().at(0).toUuid(), a.id);
        QCOMPARE(dropped.first().at(1).toString(), QStringLiteral("media"));
        QCOMPARE(dropped.first().at(2).toStringList(), QStringList{QDir::tempPath()});
    }

    // Objects of the account dropped onto a bucket: copied (Shift: moved) to its root.
    void objectsDroppedOnABucketGoThere()
    {
        Account a = addAccount(true);
        a.readOnly = false;
        QVERIFY(m_accounts->save(a));
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.resize(260, 400);
        sidebar.show();
        sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")).isValid(), 10000);
        QList<std::tuple<QString, ObjectSelection, bool>> dropped;
        connect(&sidebar, &AccountSidebar::objectsDroppedOnBucket, this,
                [&](const QUuid&, const QString& bucket, const ObjectSelection& sel, bool move) {
                    dropped.append({bucket, sel, move});
                });
        ObjectSelection sel;
        sel.account = a.id;
        sel.bucket = QStringLiteral("other");
        sel.base = QStringLiteral("docs/");
        sel.items = {{QStringLiteral("docs/a.txt"), false}};
        std::unique_ptr<QMimeData> mime(sel.toMimeData());
        const auto dropAt = [&](const QModelIndex& index, Qt::KeyboardModifiers mods, const QMimeData* data) {
            const QPoint pos = sidebar.view()->visualRect(index).center();
            QDragEnterEvent enter(DragPosition(pos), Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(sidebar.view()->viewport(), &enter);
            QDropEvent drop(QPointF(pos), Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(sidebar.view()->viewport(), &drop);
        };
        const QModelIndex media = sidebar.model()->bucketIndex(a.id, QStringLiteral("media"));
        dropAt(media, Qt::NoModifier, mime.get());
        dropAt(media, Qt::ShiftModifier, mime.get());
        QCOMPARE(dropped.size(), 2);
        QCOMPARE(std::get<0>(dropped.first()), QStringLiteral("media"));
        QCOMPARE(std::get<1>(dropped.first()).items.first().key, QStringLiteral("docs/a.txt"));
        QVERIFY(!std::get<2>(dropped.first()));
        QVERIFY(std::get<2>(dropped.last()));
        // Another account's objects: not here (between accounts comes later).
        sel.account = QUuid::createUuid();
        std::unique_ptr<QMimeData> foreign(sel.toMimeData());
        dropAt(media, Qt::NoModifier, foreign.get());
        QCOMPARE(dropped.size(), 2);
    }

    void bookmarksSectionOpensPlaces()
    {
        const Account a = addAccount(true);
        LocationStore store(m_dir.filePath(QStringLiteral("locations-%1.json").arg(m_run)));
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.resize(260, 500);
        sidebar.show();
        auto* toggle = sidebar.findChild<QToolButton*>(QStringLiteral("sectionToggle"));
        QVERIFY(toggle);
        QVERIFY(toggle->isHidden());   // no store: no section
        sidebar.setLocationStore(&store);
        QVERIFY(!toggle->isHidden());
        auto* hint = sidebar.findChild<QLabel*>(QStringLiteral("sectionHint"));
        QVERIFY(!hint->isHidden());   // none yet
        QVERIFY(sidebar.bookmarkView()->isHidden());

        store.addBookmark(QStringLiteral("Photos"), Location{a.id, QStringLiteral("media"), QStringLiteral("photos/")});
        QVERIFY(!sidebar.bookmarkView()->isHidden());
        QVERIFY(hint->isHidden());
        const QModelIndex first = sidebar.bookmarkView()->model()->index(0, 0);
        QCOMPARE(first.data().toString(), QStringLiteral("Photos"));
        QVERIFY(first.data(Qt::ToolTipRole).toString().startsWith(a.name));

        QList<Location> opened;
        connect(&sidebar, &AccountSidebar::bookmarkActivated, this, [&](const Location& l) { opened.append(l); });
        emit sidebar.bookmarkView()->clicked(first);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first(), (Location{a.id, QStringLiteral("media"), QStringLiteral("photos/")}));

        QSignalSpy expanded(&sidebar, &AccountSidebar::bookmarksExpandedChanged);
        toggle->click();
        QVERIFY(!sidebar.bookmarksExpanded());
        QVERIFY(sidebar.bookmarkView()->isHidden());
        QCOMPARE(expanded.size(), 1);
        QCOMPARE(expanded.first().first().toBool(), false);
        toggle->click();
        QVERIFY(!sidebar.bookmarkView()->isHidden());
    }

    void followsTheBrowserWithoutEchoing()
    {
        const Account a = addAccount(true);
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.resize(260, 400);
        sidebar.show();
        QSignalSpy buckets(&sidebar, &AccountSidebar::bucketSelected);
        QSignalSpy accounts(&sidebar, &AccountSidebar::accountSelected);
        sidebar.setCurrentLocation(Location{a.id, QStringLiteral("media"), QStringLiteral("x/")});
        // The buckets aren't listed yet: the account is marked.
        QCOMPARE(sidebar.view()->currentIndex(), sidebar.model()->accountIndex(a.id));
        sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")).isValid(), 10000);
        sidebar.setCurrentLocation(Location{a.id, QStringLiteral("media"), QString()});
        QCOMPARE(sidebar.view()->currentIndex(), sidebar.model()->bucketIndex(a.id, QStringLiteral("media")));
        QCOMPARE(buckets.size(), 0);
        QCOMPARE(accounts.size(), 0);
        // A click still reports.
        sidebar.view()->setCurrentIndex(sidebar.model()->accountIndex(a.id));
        QCOMPARE(accounts.size(), 1);
    }

    void asksForAMissingSecretOnce()
    {
        const Account a = addAccount(false);
        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.show();
        sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        // A refresh while the first prompt is open must not stack a second one.
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.findChildren<QInputDialog*>().size() == 1, 10000);
        sidebar.model()->refresh(sidebar.model()->accountIndex(a.id));
        QTest::qWait(300);
        QCOMPARE(sidebar.findChildren<QInputDialog*>().size(), 1);

        QInputDialog* prompt = sidebar.findChildren<QInputDialog*>().first();
        QCOMPARE(prompt->textEchoMode(), QLineEdit::Password);
        prompt->setTextValue(kSecret);
        prompt->accept();
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.model()->bucketIndex(a.id, QStringLiteral("media")).isValid(), 10000);
    }

    // QBUCKET_SCREENSHOT_DIR set: saves sidebar-dark.png (devlog).
    void screenshot()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        Account prod = addAccount(true);
        prod.name = QStringLiteral("Hetzner nbg1");
        prod.externalBuckets = {QStringLiteral("shared-reports")};
        m_accounts->save(prod);
        Account staging = addAccount(true);
        staging.name = QStringLiteral("Ceph staging");
        staging.environment = Environment::Staging;
        staging.readOnly = false;
        m_accounts->save(staging);
        Account dev = addAccount(false);
        dev.name = QStringLiteral("Garage lab");
        dev.environment = Environment::Development;
        dev.readOnly = false;
        dev.endpoint = QStringLiteral("http://127.0.0.1:1");
        m_accounts->save(dev);

        AccountSidebar sidebar(m_accounts.get(), m_secrets.get(), m_sessions.get());
        sidebar.resize(260, 300);
        sidebar.show();
        for (const Account& a : {prod, staging, dev})
            sidebar.view()->expand(sidebar.model()->accountIndex(a.id));
        QTRY_VERIFY_WITH_TIMEOUT(sidebar.model()->bucketIndex(prod.id, QStringLiteral("media")).isValid(), 10000);
        QTest::qWait(1500);   // the lab endpoint fails, the secret prompt is dismissed
        for (QInputDialog* d : sidebar.findChildren<QInputDialog*>())
            d->reject();
        sidebar.view()->setCurrentIndex(sidebar.model()->bucketIndex(prod.id, QStringLiteral("media")));
        QTest::qWait(150);
        QDir().mkpath(dir);
        QVERIFY(sidebar.grab().save(QDir(dir).filePath(QStringLiteral("sidebar-dark.png"))));

        // With bookmarks (M7).
        LocationStore store(m_dir.filePath(QStringLiteral("locations-shot.json")));
        store.addBookmark(QStringLiteral("PBS backups"), Location{prod.id, QStringLiteral("media"), QStringLiteral("pbs/vm/101/")});
        store.addBookmark(QStringLiteral("Reports 2026"), Location{prod.id, QStringLiteral("shared-reports"), QStringLiteral("2026/")});
        store.addBookmark(QStringLiteral("Lab scratch"), Location{staging.id, QStringLiteral("media"), QStringLiteral("scratch/")});
        sidebar.setLocationStore(&store);
        sidebar.resize(260, 420);
        QTest::qWait(150);
        QVERIFY(sidebar.grab().save(QDir(dir).filePath(QStringLiteral("sidebar-bookmarks.png"))));
    }

private:
    Account addAccount(bool withSecret)
    {
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("local");
        a.providerId = QStringLiteral("generic");
        a.region = QStringLiteral("test-1");
        a.endpoint = m_server->endpointUrl().toString();
        a.accessKeyId = kKeyId;
        a.readOnly = true;
        if (withSecret)
            m_keychain->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                           + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                       kSecret);
        m_accounts->save(a);
        return a;
    }

    ThemeManager m_theme;
    QTemporaryDir m_dir;
    int m_run = 0;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    testing::FakeKeychainBackend* m_keychain = nullptr;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
};

QTEST_MAIN(TestAccountSidebar)
#include "test_account_sidebar.moc"
