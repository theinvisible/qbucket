#include "accounts/AccountStore.h"

#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

Account sample(const QString& name)
{
    Account a;
    a.id = QUuid::createUuid();
    a.name = name;
    a.providerId = QStringLiteral("hetzner");
    a.region = QStringLiteral("fsn1");
    a.accessKeyId = QStringLiteral("AKIA") + name.toUpper();
    return a;
}

} // namespace

class TestAccountStore : public QObject {
    Q_OBJECT

private slots:
    void roundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("accounts.ini"));
        Account prod = sample(QStringLiteral("Production"));
        prod.addressing = s3::AddressingStyle::Path;
        prod.readOnly = true;
        prod.externalBuckets = {QStringLiteral("shared-a"), QStringLiteral("shared-b")};
        Account dev = sample(QStringLiteral("dev box"));
        dev.providerId = QStringLiteral("generic");
        dev.endpoint = QStringLiteral("http://127.0.0.1:7480");
        dev.environment = Environment::Development;
        {
            AccountStore store(path);
            QSignalSpy changed(&store, &AccountStore::accountsChanged);
            QVERIFY(store.save(prod));
            QVERIFY(store.save(dev));
            QCOMPARE(changed.size(), 2);
        }
        AccountStore reopened(path);
        QCOMPARE(reopened.accounts().size(), 2);
        QCOMPARE(reopened.accounts().first().name, QStringLiteral("dev box"));   // sorted
        const std::optional<Account> p = reopened.account(prod.id);
        QVERIFY(p);
        QCOMPARE(p->addressing, s3::AddressingStyle::Path);
        QVERIFY(p->readOnly);
        QCOMPARE(p->externalBuckets, prod.externalBuckets);
        QCOMPARE(p->environment, Environment::Production);
        QCOMPARE(reopened.account(dev.id)->endpoint, dev.endpoint);
        QCOMPARE(reopened.account(dev.id)->environment, Environment::Development);
    }

    void updateAndRemoveDoNotDisturbOthers()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("accounts.ini"));
        AccountStore store(path);
        const Account a = sample(QStringLiteral("a"));
        const Account b = sample(QStringLiteral("b"));
        const Account c = sample(QStringLiteral("c"));
        QVERIFY(store.save(a) && store.save(b) && store.save(c));
        Account renamed = b;
        renamed.name = QStringLiteral("b renamed");
        QVERIFY(store.save(renamed));
        QVERIFY(store.remove(a.id));
        QVERIFY(!store.remove(a.id));

        AccountStore reopened(path);
        QCOMPARE(reopened.accounts().size(), 2);
        QCOMPARE(reopened.account(b.id)->name, QStringLiteral("b renamed"));
        QVERIFY(reopened.account(c.id));
        QVERIFY(!reopened.account(a.id));
    }

    void refusesInvalidAccounts()
    {
        QTemporaryDir dir;
        AccountStore store(dir.filePath(QStringLiteral("accounts.ini")));
        Account a = sample(QStringLiteral("x"));
        a.accessKeyId.clear();
        QVERIFY(!store.save(a));
        a = sample(QStringLiteral("  "));
        QVERIFY(!store.save(a));
        QVERIFY(store.accounts().isEmpty());
    }

    void unknownEnvironmentIsProduction()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("accounts.ini"));
        const Account a = sample(QStringLiteral("x"));
        {
            AccountStore store(path);
            QVERIFY(store.save(a));
        }
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(QStringLiteral("account-%1/environment").arg(a.id.toString(QUuid::WithoutBraces)),
                         QStringLiteral("whatever"));
        }
        QCOMPARE(AccountStore(path).account(a.id)->environment, Environment::Production);
    }

    void noSecretsInTheFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("accounts.ini"));
        AccountStore store(path);
        QVERIFY(store.save(sample(QStringLiteral("x"))));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray content = f.readAll().toLower();
        QVERIFY(!content.contains("secret"));
        QVERIFY(!content.contains("password"));
    }
};

QTEST_GUILESS_MAIN(TestAccountStore)
#include "test_account_store.moc"
