#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "support/FakeKeychainBackend.h"
#include "ui/ImportAccountsDialog.h"
#include "ui/ThemeManager.h"

#include <QCheckBox>
#include <QDir>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

QString fixture(const char* name)
{
    return QStringLiteral(TEST_DATA_DIR "/import/") + QLatin1String(name);
}

int rowNamed(QTableWidget* table, const QString& name)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, ImportAccountsDialog::NameColumn)->text() == name)
            return row;
    }
    return -1;
}

} // namespace

class TestImportAccountsDialog : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    ThemeManager m_theme;

private slots:
    void initTestCase()
    {
        m_theme.apply(QStringLiteral("Dark"));
        QVERIFY(m_dir.isValid());
    }

    void importsWhatIsTicked()
    {
        AccountStore accounts(m_dir.filePath(QStringLiteral("accounts.ini")));
        SecretStore secrets(std::make_unique<testing::FakeKeychainBackend>());
        ImportAccountsDialog dlg(&accounts, &secrets);
        dlg.show();
        dlg.load(AccountImporter::Source::Aws, {fixture("credentials"), fixture("config")});
        QTableWidget* table = dlg.table();
        QTRY_COMPARE(table->rowCount(), 5);

        const int hetzner = rowNamed(table, QStringLiteral("hetzner-nbg"));
        const int sso = rowNamed(table, QStringLiteral("sso"));
        const int minio = rowNamed(table, QStringLiteral("minio"));
        const int def = rowNamed(table, QStringLiteral("default"));
        QVERIFY(hetzner >= 0 && sso >= 0 && minio >= 0 && def >= 0);
        auto* provider = qobject_cast<QComboBox*>(table->cellWidget(hetzner, ImportAccountsDialog::ProviderColumn));
        QCOMPARE(provider->currentData().toString(), QStringLiteral("hetzner"));
        // Not importable: unticked and can't be ticked.
        QCOMPARE(table->item(sso, ImportAccountsDialog::ImportColumn)->checkState(), Qt::Unchecked);
        QVERIFY(!(table->item(sso, ImportAccountsDialog::ImportColumn)->flags() & Qt::ItemIsUserCheckable));

        // Only Hetzner and MinIO, the latter as Ceph and renamed.
        table->item(def, ImportAccountsDialog::ImportColumn)->setCheckState(Qt::Unchecked);
        table->item(minio, ImportAccountsDialog::NameColumn)->setText(QStringLiteral("Lab"));
        auto* minioProvider = qobject_cast<QComboBox*>(table->cellWidget(minio, ImportAccountsDialog::ProviderColumn));
        minioProvider->setCurrentIndex(minioProvider->findData(QStringLiteral("ceph")));
        QVERIFY(dlg.importButton()->isEnabled());
        QVERIFY(dlg.importButton()->text().contains(QLatin1Char('2')));

        QSignalSpy imported(&dlg, &ImportAccountsDialog::imported);
        QSignalSpy accepted(&dlg, &QDialog::accepted);
        dlg.importButton()->click();
        QTRY_COMPARE(accepted.size(), 1);
        QCOMPARE(imported.first().at(0).toInt(), 2);
        QCOMPARE(accounts.accounts().size(), 2);
        bool lab = false;
        for (const Account& a : accounts.accounts()) {
            QCOMPARE(a.environment, Environment::Production);
            QVERIFY(a.readOnly);   // Production: read-only unless switched off
            if (a.name == QLatin1String("Lab")) {
                lab = true;
                QCOMPARE(a.providerId, QStringLiteral("ceph"));
                QCOMPARE(a.endpoint, QStringLiteral("http://localhost:9000"));
            }
        }
        QVERIFY(lab);
    }

    // QBUCKET_SCREENSHOT_DIR set: saves import-accounts.png (devlog).
    void screenshot()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        AccountStore accounts(m_dir.filePath(QStringLiteral("accounts-shot.ini")));
        SecretStore secrets(std::make_unique<testing::FakeKeychainBackend>());
        ImportAccountsDialog dlg(&accounts, &secrets);
        dlg.resize(980, 560);
        dlg.show();
        dlg.load(AccountImporter::Source::Aws, {fixture("credentials"), fixture("config")});
        QTRY_COMPARE(dlg.table()->rowCount(), 5);
        QTest::qWait(150);
        QDir().mkpath(dir);
        QVERIFY(dlg.grab().save(QDir(dir).filePath(QStringLiteral("import-accounts.png"))));
    }

    void aMissingFileSaysSo()
    {
        AccountStore accounts(m_dir.filePath(QStringLiteral("accounts-2.ini")));
        SecretStore secrets(std::make_unique<testing::FakeKeychainBackend>());
        ImportAccountsDialog dlg(&accounts, &secrets);
        dlg.load(AccountImporter::Source::Rclone, {fixture("rclone-encrypted.conf")});
        QTRY_VERIFY(dlg.statusLabel()->text().contains(QLatin1String("encrypted")));
        QCOMPARE(dlg.table()->rowCount(), 0);
        QVERIFY(!dlg.importButton()->isEnabled());
    }
};

QTEST_MAIN(TestImportAccountsDialog)
#include "test_import_accounts_dialog.moc"
