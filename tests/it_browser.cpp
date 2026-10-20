// The real main window against a real S3 server — opt-in like
// it_operations (QBUCKET_IT_*), and read-only: the account is marked
// read-only, so the gate refuses every change. Opens the bucket, lists it,
// opens guard/ (or the first folder), shows an object's details, checks
// that the capabilities were probed. With QBUCKET_SCREENSHOT_DIR set (and a
// real platform for real fonts) it saves browser-<theme>.png.

#include "controllers/BrowseController.h"
#include "controllers/SettingsController.h"
#include "support/FakeKeychainBackend.h"
#include "ui/DetailPane.h"
#include "ui/MainWindow.h"
#include "ui/ObjectBrowser.h"
#include "ui/ThemeManager.h"

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"

#include <QAction>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTranslator>
#include <QTreeView>
#include <QtTest>

using namespace qbucket;

namespace {

QString env(const char* name)
{
    return qEnvironmentVariable(name);
}

} // namespace

class ItBrowser : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (env("QBUCKET_IT_ENDPOINT").isEmpty())
            QSKIP("QBUCKET_IT_ENDPOINT not set — integration tests are opt-in");
        const QString bucket = env("QBUCKET_IT_BUCKET");
        static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
        QVERIFY2(safeName.match(bucket).hasMatch(), "refusing to run: QBUCKET_IT_BUCKET must be a qbucket-it-* bucket");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("qbucket-it-browser"));
        QCoreApplication::setOrganizationName(QStringLiteral("qbucket-test"));
        QVERIFY(m_dir.isValid());

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts.ini")));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = env("QBUCKET_IT_BACKEND") == QLatin1String("hetzner")
                     ? QStringLiteral("Hetzner %1").arg(env("QBUCKET_IT_REGION"))
                     : env("QBUCKET_IT_BACKEND");
        a.providerId = env("QBUCKET_IT_PROVIDER").isEmpty() ? QStringLiteral("generic") : env("QBUCKET_IT_PROVIDER");
        a.region = env("QBUCKET_IT_REGION");
        a.endpoint = env("QBUCKET_IT_ENDPOINT");
        a.addressing = env("QBUCKET_IT_ADDRESSING") == QLatin1String("virtual") ? s3::AddressingStyle::VirtualHosted
                                                                                : s3::AddressingStyle::Path;
        a.accessKeyId = env("QBUCKET_IT_ACCESS_KEY");
        a.environment = Environment::Production;
        a.readOnly = true;   // nothing in here may change anything
        a.externalBuckets = {bucket};
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                env("QBUCKET_IT_SECRET_KEY"));
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        QVERIFY(m_accounts->save(a));
        m_account = a;
        m_bucket = bucket;
        // As the app does: English plural forms ("1 object").
        if (m_plurals.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")))
            QCoreApplication::installTranslator(&m_plurals);
        m_settings = std::make_unique<SettingsController>();
        m_theme = std::make_unique<ThemeManager>();
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_secrets.reset();
    }

    void browse_data()
    {
        QTest::addColumn<QString>("theme");
        QTest::newRow("dark") << QStringLiteral("Dark");
        QTest::newRow("light") << QStringLiteral("Light");
    }

    void browse()
    {
        QFETCH(QString, theme);
        m_theme->apply(theme);
        m_window = std::make_unique<MainWindow>(m_settings.get(), m_theme.get(), m_accounts.get(), m_secrets.get());
        m_window->resize(1280, 760);
        m_window->show();
        auto* browser = m_window->findChild<ObjectBrowser*>();
        QVERIFY(browser);

        // The sidebar lists the account's buckets.
        auto* tree = m_window->findChild<QTreeView*>(QStringLiteral("accountTree"));
        QVERIFY(tree);
        tree->expand(tree->model()->index(0, 0));

        browser->openBucket(m_account.id, m_bucket);
        QTRY_VERIFY_WITH_TIMEOUT(browser->model()->state() == ObjectTableModel::State::Complete
                                     || browser->model()->state() == ObjectTableModel::State::Ready, 30000);
        QVERIFY(browser->model()->rowCount() > 0);

        // Into guard/ (it holds the guard object), else the first folder.
        int row = browser->model()->rowOfKey(QStringLiteral("guard/"));
        if (row < 0)
            row = 0;
        if (browser->model()->index(row, 0).data(ObjectTableModel::IsFolderRole).toBool()) {
            browser->navigation()->navigate({m_account.id, m_bucket, browser->model()->index(row, 0)
                                                                         .data(ObjectTableModel::KeyRole).toString()});
            QTRY_VERIFY_WITH_TIMEOUT(browser->model()->state() == ObjectTableModel::State::Complete, 30000);
        }
        QVERIFY(browser->model()->rowCount() > 0);
        browser->view()->selectRow(0);
        auto* details = m_window->findChild<DetailPane*>();
        QVERIFY(details);
        QTRY_VERIFY_WITH_TIMEOUT(!details->meta().key.isEmpty(), 30000);

        // Read-only: no change is offered.
        QVERIFY(!browser->deleteAction()->isEnabled());
        QVERIFY(!browser->renameAction()->isEnabled());

        QTest::qWait(1500);   // capability probes, sidebar listing
        const QString dir = env("QBUCKET_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            QVERIFY(m_window->grab().save(QDir(dir).filePath(QStringLiteral("browser-%1.png").arg(theme.toLower()))));
        }
        m_window.reset();
    }

private:
    QTemporaryDir m_dir;
    QTranslator m_plurals;
    QString m_bucket;
    Account m_account;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<SettingsController> m_settings;
    std::unique_ptr<ThemeManager> m_theme;
    std::unique_ptr<MainWindow> m_window;
};

QTEST_MAIN(ItBrowser)
#include "it_browser.moc"
