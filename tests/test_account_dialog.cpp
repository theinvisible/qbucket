#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "support/FakeKeychainBackend.h"
#include "ui/AccountDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QCheckBox>
#include <QDir>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;

// Fields are found by placeholder text or type rather than by position, so
// layout changes don't break the test.
class TestAccountDialog : public QObject {
    Q_OBJECT

private:
    static QLineEdit* byPlaceholder(AccountDialog& d, const QString& start)
    {
        for (QLineEdit* e : d.findChildren<QLineEdit*>()) {
            if (e->placeholderText().startsWith(start))
                return e;
        }
        return nullptr;
    }
    static QComboBox* comboWithItem(AccountDialog& d, const QString& text)
    {
        for (QComboBox* c : d.findChildren<QComboBox*>()) {
            if (c->findText(text) >= 0)
                return c;
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void hetznerIsTheDefaultPreset()
    {
        QTemporaryDir dir;
        AccountStore accounts(dir.filePath(QStringLiteral("a.ini")));
        SecretStore secrets{std::make_unique<testing::FakeKeychainBackend>()};
        AccountDialog d(&accounts, &secrets);

        QComboBox* provider = comboWithItem(d, QStringLiteral("Hetzner Object Storage"));
        QVERIFY(provider);
        QCOMPARE(provider->currentText(), QStringLiteral("Hetzner Object Storage"));
        QComboBox* region = comboWithItem(d, QStringLiteral("fsn1"));
        QVERIFY(region);
        QLineEdit* endpoint = byPlaceholder(d, QStringLiteral("https://"));
        QCOMPARE(endpoint->text(), QStringLiteral("https://fsn1.your-objectstorage.com"));
        region->setCurrentText(QStringLiteral("hel1"));
        QCOMPARE(endpoint->text(), QStringLiteral("https://hel1.your-objectstorage.com"));
        // The endpoint follows the region unless typed in, and isn't stored then.
        QVERIFY(d.account().endpoint.isEmpty());
        QCOMPARE(d.account().region, QStringLiteral("hel1"));

        // Production starts read-only.
        QCheckBox* readOnly = d.findChild<QCheckBox*>();
        QVERIFY(readOnly->isChecked());
        QComboBox* env = comboWithItem(d, QStringLiteral("Development"));
        env->setCurrentIndex(env->findText(QStringLiteral("Development")));
        QVERIFY(!readOnly->isChecked());

        // A generic endpoint has to be entered.
        provider->setCurrentIndex(provider->findData(QStringLiteral("generic")));
        QVERIFY(endpoint->text().isEmpty());
    }

    void testConnectionAndSave()
    {
        s3::Credentials c;
        c.accessKeyId = QStringLiteral("AKIDDIALOG");
        c.secretAccessKey = QStringLiteral("dialog-secret");
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(c, "test-1");
        server.on("GET", QStringLiteral("^/$"), FakeResponse::ok("<ListAllMyBucketsResult><Buckets/></ListAllMyBucketsResult>"));

        QTemporaryDir dir;
        AccountStore accounts(dir.filePath(QStringLiteral("a.ini")));
        auto* backend = new testing::FakeKeychainBackend;
        SecretStore secrets{std::unique_ptr<KeychainBackend>(backend)};
        AccountDialog d(&accounts, &secrets);
        QSignalSpy saved(&d, &AccountDialog::accountSaved);

        QComboBox* provider = comboWithItem(d, QStringLiteral("Hetzner Object Storage"));
        provider->setCurrentIndex(provider->findData(QStringLiteral("generic")));
        QComboBox* region = d.findChildren<QComboBox*>().value(1);
        region->setEditText(QStringLiteral("test-1"));
        QLineEdit* endpoint = byPlaceholder(d, QStringLiteral("https://"));
        endpoint->setText(server.endpointUrl().toString());
        emit endpoint->textEdited(endpoint->text());
        byPlaceholder(d, QStringLiteral("e.g."))->setText(QStringLiteral("Local RGW"));
        QLineEdit* secret = byPlaceholder(d, QStringLiteral("Stored in"));
        secret->setText(c.secretAccessKey);
        QLineEdit* key = nullptr;
        for (QLineEdit* e : d.findChildren<QLineEdit*>()) {
            if (e->placeholderText().isEmpty() && e->echoMode() == QLineEdit::Normal && e != region->lineEdit())
                key = e;
        }
        QVERIFY(key);
        key->setText(c.accessKeyId);

        QPushButton* test = nullptr;
        for (QPushButton* b : d.findChildren<QPushButton*>()) {
            if (b->text() == AccountDialog::tr("Test Connection"))
                test = b;
        }
        QVERIFY(test && test->isEnabled());
        test->click();
        QLabel* status = d.findChild<QLabel*>(QStringLiteral("emptyState"));
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QLatin1String("Connected")), 10000);

        QPushButton* save = d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QVERIFY(save->isEnabled());
        save->click();
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(d.result(), int(QDialog::Accepted));

        const Account a = saved.first().first().value<Account>();
        QCOMPARE(accounts.accounts().size(), 1);
        QCOMPARE(accounts.accounts().first().name, QStringLiteral("Local RGW"));
        QCOMPARE(accounts.accounts().first().endpoint, server.endpointUrl().toString());
        QCOMPARE(backend->entries.value(QStringLiteral("QBucket|")
                                        + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey)),
                 c.secretAccessKey);
    }

    // QBUCKET_SCREENSHOT_DIR: save account-dialog-{dark,light}.png (run on a
    // real platform for real fonts, like test_main_window).
    void screenshots()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        QTemporaryDir tmp;
        AccountStore accounts(tmp.filePath(QStringLiteral("a.ini")));
        SecretStore secrets{std::make_unique<testing::FakeKeychainBackend>()};
        ThemeManager theme;
        QDir().mkpath(dir);
        for (const QString& name : {QStringLiteral("Dark"), QStringLiteral("Light")}) {
            theme.apply(name);
            AccountDialog d(&accounts, &secrets);
            byPlaceholder(d, QStringLiteral("e.g."))->setText(QStringLiteral("Hetzner production"));
            d.show();
            QVERIFY(QTest::qWaitForWindowExposed(&d));
            QTest::qWait(150);
            QVERIFY(d.grab().save(QDir(dir).filePath(QStringLiteral("account-dialog-%1.png").arg(name.toLower()))));
        }
    }

    void keychainFailureSavesNothing()
    {
        QTemporaryDir dir;
        AccountStore accounts(dir.filePath(QStringLiteral("a.ini")));
        auto* backend = new testing::FakeKeychainBackend;
        backend->mode = KeychainBackend::Status::AccessDenied;
        SecretStore secrets{std::unique_ptr<KeychainBackend>(backend)};
        AccountDialog d(&accounts, &secrets);
        byPlaceholder(d, QStringLiteral("e.g."))->setText(QStringLiteral("Prod"));
        byPlaceholder(d, QStringLiteral("Stored in"))->setText(QStringLiteral("s"));
        for (QLineEdit* e : d.findChildren<QLineEdit*>()) {
            if (e->placeholderText().isEmpty() && e->echoMode() == QLineEdit::Normal
                && e->parentWidget() == &d)
                e->setText(QStringLiteral("AKID"));
        }
        QPushButton* save = d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QVERIFY(save->isEnabled());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("keychain write failed")));
        save->click();
        QLabel* status = d.findChild<QLabel*>(QStringLiteral("emptyState"));
        QTRY_VERIFY(status->property("error").toBool());
        QVERIFY(accounts.accounts().isEmpty());
        QVERIFY(d.result() != int(QDialog::Accepted));
    }
};

QTEST_MAIN(TestAccountDialog)
#include "test_account_dialog.moc"
