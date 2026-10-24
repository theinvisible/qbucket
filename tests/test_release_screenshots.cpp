// The pictures for the README and the AppStream metainfo (docs/img/): the
// real main window on a FakeObjectStore with believable content — a bucket
// with folders and files, an object's details, the versions view — in the
// dark and the light theme. Runs only with QBUCKET_SCREENSHOT_DIR set, on a
// real platform:
//   QT_QPA_PLATFORM=xcb QBUCKET_SCREENSHOT_DIR=docs/img build/tests/test_release_screenshots

#include "controllers/BrowseController.h"
#include "controllers/SettingsController.h"
#include "support/AccountHarness.h"
#include "ui/AccountSidebar.h"
#include "ui/MainWindow.h"
#include "ui/ObjectBrowser.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QDir>
#include <QDockWidget>
#include <QStandardPaths>
#include <QTableView>
#include <QTranslator>
#include <QTreeView>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestReleaseScreenshots : public QObject {
    Q_OBJECT

private:
    QString m_dir;
    QTranslator m_plurals;
    std::unique_ptr<SettingsController> m_settings;
    std::unique_ptr<ThemeManager> m_theme;
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<MainWindow> m_window;
    ObjectBrowser* m_browser = nullptr;

    static QByteArray bytes(qsizetype size, char fill) { return QByteArray(size, fill); }

    void fill()
    {
        m_objects->createBucket(QStringLiteral("backups"));
        m_objects->createBucket(QStringLiteral("website"));
        const QString b = QStringLiteral("media-assets");
        m_objects->createBucket(b);
        m_objects->setVersioning(b, QStringLiteral("Enabled"));
        for (const char* folder : {"brand/", "campaigns/", "photos/", "videos/"})
            m_objects->putObject(b, QString::fromLatin1(folder), QByteArray());
        m_objects->putObject(b, QStringLiteral("brand/logo.svg"), bytes(5200, 's'), QStringLiteral("image/svg+xml"));
        m_objects->putObject(b, QStringLiteral("photos/team-2026.jpg"), bytes(3'400'000, 'j'), QStringLiteral("image/jpeg"));
        const struct {
            const char* key;
            qsizetype size;
            const char* type;
        } files[] = {
            {"brand-guidelines.pdf", 2'480'000, "application/pdf"},
            {"hero-banner.jpg", 1'210'000, "image/jpeg"},
            {"price-list-2026.xlsx", 86'000, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
            {"product-launch.mp4", 48'200'000, "video/mp4"},
            {"press-kit.zip", 12'700'000, "application/zip"},
            {"README.md", 2'100, "text/markdown"},
        };
        for (const auto& f : files)
            m_objects->putObject(b, QString::fromLatin1(f.key), bytes(f.size, 'x'), QString::fromLatin1(f.type));
        // Versions of the price list: two earlier states, one deleted file.
        m_objects->putObject(b, QStringLiteral("price-list-2026.xlsx"), bytes(84'500, 'y'),
                             QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"));
        m_objects->putObject(b, QStringLiteral("price-list-2026.xlsx"), bytes(86'000, 'z'),
                             QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"));
        m_objects->putObject(b, QStringLiteral("old-banner.png"), bytes(184'000, 'p'), QStringLiteral("image/png"));
        m_objects->deleteObject(b, QStringLiteral("old-banner.png"));
    }

    void shoot(const QString& theme, const QString& fileName)
    {
        m_theme->apply(theme);
        QTest::qWait(300);
        QVERIFY(m_window->grab().save(QDir(m_dir).filePath(fileName)));
    }

    void selectRow(const QString& name)
    {
        QAbstractItemModel* model = m_browser->view()->model();
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->index(r, 0).data().toString() == name) {
                m_browser->view()->selectRow(r);
                return;
            }
        }
        QFAIL(qPrintable(QStringLiteral("no row %1").arg(name)));
    }

private slots:
    void initTestCase()
    {
        m_dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (m_dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        QDir().mkpath(m_dir);
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("qbucket-test-screenshots"));
        QCoreApplication::setOrganizationName(QStringLiteral("qbucket-test"));
        if (m_plurals.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")))   // as the app does
            QCoreApplication::installTranslator(&m_plurals);
        // English numbers and dates, whatever the machine's locale.
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        m_settings = std::make_unique<SettingsController>();
        m_theme = std::make_unique<ThemeManager>();

        m_h = std::make_unique<AccountHarness>(QStringLiteral("Company storage"));
        QVERIFY(m_h->ok());
        Account a = m_h->account;
        a.providerId = QStringLiteral("aws");
        a.addressing = s3::AddressingStyle::Path;
        a.environment = Environment::Production;
        a.readOnly = false;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        fill();

        m_window = std::make_unique<MainWindow>(m_settings.get(), m_theme.get(), m_h->accounts.get(),
                                                m_h->secrets.get());
        m_window->resize(1280, 800);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_browser = m_window->findChild<ObjectBrowser*>();
        QVERIFY(m_browser);
        // The list gets the room; the (empty) transfer panel a strip.
        auto* dock = m_window->findChild<QDockWidget*>(QStringLiteral("transferDock"));
        QVERIFY(dock);
        m_window->resizeDocks({dock}, {200}, Qt::Vertical);
        // The account's buckets in the sidebar.
        auto* sidebar = m_window->findChild<AccountSidebar*>();
        QVERIFY(sidebar);
        sidebar->view()->expandAll();
        QTRY_VERIFY_WITH_TIMEOUT(sidebar->view()->model()->rowCount(sidebar->view()->model()->index(0, 0)) == 3, 10000);
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_objects.reset();
        m_h.reset();
    }

    void mainWindow()
    {
        m_browser->openBucket(m_h->account.id, QStringLiteral("media-assets"));
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
        m_browser->view()->setFocus();
        selectRow(QStringLiteral("brand-guidelines.pdf"));
        QTest::qWait(500);   // the details pane's HEAD
        shoot(QStringLiteral("Dark"), QStringLiteral("main-dark.png"));
        shoot(QStringLiteral("Light"), QStringLiteral("main-light.png"));
    }

    void versions()
    {
        m_settings->setBrowseMode(QStringLiteral("versions"));   // as the toolbar does
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->mode(), ObjectTableModel::Mode::Versions, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
        selectRow(QStringLiteral("price-list-2026.xlsx"));
        QTest::qWait(500);
        shoot(QStringLiteral("Dark"), QStringLiteral("versions-dark.png"));
        m_settings->setBrowseMode(QStringLiteral("folders"));
    }
};

QTEST_MAIN(TestReleaseScreenshots)
#include "test_release_screenshots.moc"
