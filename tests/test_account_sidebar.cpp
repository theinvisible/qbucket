#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/AccountTreeModel.h"
#include "support/FakeKeychainBackend.h"
#include "ui/AccountSidebar.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QApplication>
#include <QDir>
#include <QInputDialog>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;

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
