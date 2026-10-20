#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/BrowseController.h"
#include "support/FakeBucket.h"
#include "support/FakeKeychainBackend.h"
#include "ui/ObjectBrowser.h"
#include "ui/PathBar.h"
#include "ui/ThemeManager.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTableView>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::FakeBucket;

namespace {

const QString kKeyId = QStringLiteral("AKIDBROWSER");
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

        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("docs/sub/\ndocs/a b.txt\ndocs/c.txt"));
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(QGuiApplication::clipboard()->text().startsWith(QLatin1String("s3://bkt/docs/sub/\n")));

        view->setCurrentIndex(browser.model()->index(0, 0));   // the folder alone
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
