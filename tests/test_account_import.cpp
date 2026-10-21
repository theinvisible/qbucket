#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountImporter.h"
#include "support/FakeKeychainBackend.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

QString fixture(const char* name)
{
    return QStringLiteral(TEST_DATA_DIR "/import/") + QLatin1String(name);
}

QByteArray read(const char* name)
{
    QFile f(fixture(name));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const ImportCandidate* byOrigin(const QList<ImportCandidate>& list, const QString& name)
{
    for (const ImportCandidate& c : list) {
        if (c.origin.contains(QStringLiteral("“%1”").arg(name)))
            return &c;
    }
    return nullptr;
}

} // namespace

class TestAccountImport : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_run = 0;

private slots:
    void awsProfilesWithSuggestedProviders()
    {
        const QList<ImportCandidate> list =
            AccountImporter::fromAws(s3::AwsConfigFiles::parse(read("credentials"), read("config")), {});
        QCOMPARE(list.size(), 5);

        const ImportCandidate* aws = byOrigin(list, QStringLiteral("default"));
        QVERIFY(aws);
        QCOMPARE(aws->providerId, QStringLiteral("aws"));
        QCOMPARE(aws->region, QStringLiteral("eu-central-1"));
        QVERIFY(aws->endpoint.isEmpty());
        QVERIFY(aws->importable());

        // The endpoint is Hetzner's preset: the profile with its quirks, no
        // endpoint stored (it follows the profile).
        const ImportCandidate* hetzner = byOrigin(list, QStringLiteral("hetzner-nbg"));
        QVERIFY(hetzner);
        QCOMPARE(hetzner->providerId, QStringLiteral("hetzner"));
        QCOMPARE(hetzner->region, QStringLiteral("nbg1"));
        QVERIFY(hetzner->endpoint.isEmpty());
        QCOMPARE(hetzner->secretAccessKey, QStringLiteral("hetzner-secret"));

        const ImportCandidate* minio = byOrigin(list, QStringLiteral("minio"));
        QVERIFY(minio);
        QCOMPARE(minio->providerId, QStringLiteral("generic"));
        QCOMPARE(minio->endpoint, QStringLiteral("http://localhost:9000"));
        QCOMPARE(minio->addressing, s3::AddressingStyle::Path);
        QCOMPARE(minio->region, QStringLiteral("us-east-1"));

        // Temporary or indirect credentials: offered, but not importable.
        QVERIFY(!byOrigin(list, QStringLiteral("temporary"))->importable());
        const ImportCandidate* sso = byOrigin(list, QStringLiteral("sso"));
        QVERIFY(!sso->importable());
        QVERIFY(sso->skipReason.contains(QLatin1String("sso_session")));
        // No secret ends up in a text meant for the screen.
        for (const ImportCandidate& c : list) {
            QVERIFY(!c.why.contains(QLatin1String("secret")));
            QVERIFY(!c.skipReason.contains(QLatin1String("secret")));
        }
    }

    void rcloneRemotes()
    {
        const RcloneConfigReader::Result config = RcloneConfigReader::parse(read("rclone.conf"));
        QVERIFY(!config.encrypted);
        QCOMPARE(config.remotes.size(), 4);   // not the drive remote
        const QList<ImportCandidate> list = AccountImporter::fromRclone(config, {});

        const ImportCandidate* hetzner = byOrigin(list, QStringLiteral("hetzner"));
        QCOMPARE(hetzner->providerId, QStringLiteral("hetzner"));   // "fsn1.your-objectstorage.com" with https://
        QCOMPARE(hetzner->region, QStringLiteral("fsn1"));
        QCOMPARE(hetzner->addressing, s3::AddressingStyle::Auto);    // not set: the profile's default

        const ImportCandidate* aws = byOrigin(list, QStringLiteral("aws-backup"));
        QCOMPARE(aws->providerId, QStringLiteral("aws"));
        QCOMPARE(aws->region, QStringLiteral("eu-west-1"));

        const ImportCandidate* ceph = byOrigin(list, QStringLiteral("ceph-lab"));
        QCOMPARE(ceph->providerId, QStringLiteral("ceph"));
        QCOMPARE(ceph->endpoint, QStringLiteral("http://ceph.lab:7480"));
        QCOMPARE(ceph->addressing, s3::AddressingStyle::Path);

        const ImportCandidate* instance = byOrigin(list, QStringLiteral("instance"));
        QVERIFY(!instance->importable());
        QVERIFY(instance->skipReason.contains(QLatin1String("env_auth")));

        QVERIFY(RcloneConfigReader::parse(read("rclone-encrypted.conf")).encrypted);
    }

    void existingAccountsAreRecognisedAndNamesStayUnique()
    {
        Account mine;
        mine.id = QUuid::createUuid();
        mine.name = QStringLiteral("default");
        mine.providerId = QStringLiteral("hetzner");
        mine.region = QStringLiteral("nbg1");
        mine.accessKeyId = QStringLiteral("HETZNERKEY");
        const QList<ImportCandidate> list =
            AccountImporter::fromAws(s3::AwsConfigFiles::parse(read("credentials"), read("config")), {mine});
        QCOMPARE(byOrigin(list, QStringLiteral("hetzner-nbg"))->existing, mine.id);
        QVERIFY(byOrigin(list, QStringLiteral("minio"))->existing.isNull());
        QCOMPARE(byOrigin(list, QStringLiteral("default"))->name, QStringLiteral("default (2)"));

        // The same key with an explicit endpoint equal to the preset is the same account.
        mine.endpoint = QStringLiteral("https://NBG1.your-objectstorage.com/");
        const QList<ImportCandidate> again =
            AccountImporter::fromAws(s3::AwsConfigFiles::parse(read("credentials"), read("config")), {mine});
        QCOMPARE(byOrigin(again, QStringLiteral("hetzner-nbg"))->existing, mine.id);
    }

    void readAndSaveKeepSecretsInTheKeychainOnly()
    {
        QVERIFY(m_dir.isValid());
        const QString ini = m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run));
        AccountStore accounts(ini);
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        testing::FakeKeychainBackend* keychain = backend.get();
        SecretStore secrets(std::move(backend));
        AccountImporter importer(&accounts, &secrets);

        QSignalSpy ready(&importer, &AccountImporter::candidatesReady);
        importer.read(AccountImporter::Source::Rclone, {fixture("rclone.conf")});
        QVERIFY(ready.wait(5000));
        const auto candidates = ready.first().at(0).value<QList<ImportCandidate>>();
        QVERIFY(ready.first().at(1).toString().isEmpty());
        QCOMPARE(candidates.size(), 4);

        QSignalSpy saved(&importer, &AccountImporter::saveFinished);
        importer.save(candidates, Environment::Staging, false);   // the skipped one is skipped
        QVERIFY(saved.wait(5000));
        QCOMPARE(saved.first().at(0).toInt(), 3);
        QVERIFY(saved.first().at(2).toStringList().isEmpty());
        QCOMPARE(accounts.accounts().size(), 3);
        for (const Account& a : accounts.accounts()) {
            QCOMPARE(a.environment, Environment::Staging);
            QVERIFY(!a.readOnly);
            QVERIFY(keychain->entries.contains(QString::fromLatin1(SecretStore::kService) + u'|'
                                               + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey)));
        }
        QFile f(ini);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray stored = f.readAll();
        QVERIFY(stored.contains("RCLONEHETZNER"));
        QVERIFY(!stored.contains("rclone-hetzner-secret"));
        QVERIFY(!stored.contains("secret"));

        // A refusing keychain: nothing is saved, each says why.
        keychain->mode = KeychainBackend::Status::AccessDenied;
        saved.clear();
        importer.save(candidates, Environment::Production, true);
        QVERIFY(saved.wait(5000));
        QCOMPARE(saved.first().at(0).toInt(), 0);
        QCOMPARE(saved.first().at(2).toStringList().size(), 3);
        QCOMPARE(accounts.accounts().size(), 3);
    }

    void unreadableOrEncryptedFilesSayWhy()
    {
        AccountStore accounts(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run)));
        SecretStore secrets(std::make_unique<testing::FakeKeychainBackend>());
        AccountImporter importer(&accounts, &secrets);
        QSignalSpy ready(&importer, &AccountImporter::candidatesReady);
        importer.read(AccountImporter::Source::Rclone, {fixture("rclone-encrypted.conf")});
        QVERIFY(ready.wait(5000));
        QVERIFY(ready.first().at(1).toString().contains(QLatin1String("encrypted")));
        importer.read(AccountImporter::Source::Aws, {fixture("does-not-exist")});
        QVERIFY(ready.wait(5000));
        QVERIFY(!ready.last().at(1).toString().isEmpty());
        importer.read(AccountImporter::Source::Aws, {});
        QVERIFY(ready.wait(5000));
        QVERIFY(!ready.last().at(1).toString().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestAccountImport)
#include "test_account_import.moc"
