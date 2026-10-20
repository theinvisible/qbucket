#include <qbucket/s3/Credentials.h>

#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;

namespace {

const QByteArray kFile =
    "# comment\n"
    "[default]\n"
    "aws_access_key_id = AKIDDEFAULT\n"
    "aws_secret_access_key = se/cr+et\\\\with=equals==\n"
    "\n"
    "; other comment\n"
    "[profile hetzner]\n"
    "aws_access_key_id=HETZNERKEY\n"
    "aws_secret_access_key=hetzner-secret\n"
    "aws_session_token=tok\n"
    "[broken]\n"
    "aws_access_key_id=ONLYKEY\n";

} // namespace

class TestCredentials : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesProfiles()
    {
        Result<Credentials> c = AwsSharedFileCredentialsProvider::parse(kFile, QStringLiteral("default"));
        QVERIFY(c);
        QCOMPARE(c->accessKeyId, QStringLiteral("AKIDDEFAULT"));
        QCOMPARE(c->secretAccessKey, QStringLiteral("se/cr+et\\\\with=equals=="));

        c = AwsSharedFileCredentialsProvider::parse(kFile, QStringLiteral("hetzner"));
        QVERIFY(c);
        QCOMPARE(c->sessionToken, QStringLiteral("tok"));

        c = AwsSharedFileCredentialsProvider::parse(kFile, QStringLiteral("broken"));
        QCOMPARE(c.error().category, ErrorCategory::Credentials);
        c = AwsSharedFileCredentialsProvider::parse(kFile, QStringLiteral("missing"));
        QCOMPARE(c.error().category, ErrorCategory::Credentials);
    }

    void readsTheFileOnEveryFetch()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("credentials"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kFile);
        f.close();

        AwsSharedFileCredentialsProvider provider(path, QStringLiteral("default"));
        std::optional<Result<Credentials>> out;
        provider.fetch([&](Result<Credentials> r) { out.emplace(std::move(r)); });
        QVERIFY(out && *out);
        QCOMPARE((*out)->accessKeyId, QStringLiteral("AKIDDEFAULT"));

        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("[default]\naws_access_key_id=ROTATED\naws_secret_access_key=s\n");
        f.close();
        provider.fetch([&](Result<Credentials> r) { out.emplace(std::move(r)); });
        QCOMPARE((*out)->accessKeyId, QStringLiteral("ROTATED"));

        AwsSharedFileCredentialsProvider missing(dir.filePath(QStringLiteral("nope")));
        missing.fetch([&](Result<Credentials> r) { out.emplace(std::move(r)); });
        QCOMPARE(out->error().category, ErrorCategory::Credentials);
    }

    void environmentProvider()
    {
        qputenv("AWS_ACCESS_KEY_ID", "ENVKEY");
        qputenv("AWS_SECRET_ACCESS_KEY", "ENVSECRET");
        qunsetenv("AWS_SESSION_TOKEN");
        std::optional<Result<Credentials>> out;
        EnvCredentialsProvider().fetch([&](Result<Credentials> r) { out.emplace(std::move(r)); });
        QVERIFY(out && *out);
        QCOMPARE((*out)->accessKeyId, QStringLiteral("ENVKEY"));
        qunsetenv("AWS_SECRET_ACCESS_KEY");
        EnvCredentialsProvider().fetch([&](Result<Credentials> r) { out.emplace(std::move(r)); });
        QCOMPARE(out->error().category, ErrorCategory::Credentials);
    }

    void expiry()
    {
        Credentials c;
        c.accessKeyId = QStringLiteral("a");
        c.secretAccessKey = QStringLiteral("b");
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(!c.isExpired(now));
        c.expiresAt = now.addSecs(30);
        QVERIFY(c.isExpired(now));           // within the 60 s margin
        c.expiresAt = now.addSecs(3600);
        QVERIFY(!c.isExpired(now));
    }
};

QTEST_GUILESS_MAIN(TestCredentials)
#include "test_credentials.moc"
