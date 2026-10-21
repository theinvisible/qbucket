#include <qbucket/s3/Credentials.h>

#include "core/AwsIni.h"

#include <QDir>
#include <QFile>
#include <QtGlobal>

namespace qbucket::s3 {

CredentialsProvider::~CredentialsProvider() = default;

StaticCredentialsProvider::StaticCredentialsProvider(Credentials credentials)
    : m_credentials(std::move(credentials))
{
}

void StaticCredentialsProvider::fetch(Callback done)
{
    if (!m_credentials.isValid()) {
        done(Error::make(ErrorCategory::Credentials, QStringLiteral("credentials are incomplete")));
        return;
    }
    done(m_credentials);
}

void EnvCredentialsProvider::fetch(Callback done)
{
    Credentials c;
    c.accessKeyId = qEnvironmentVariable("AWS_ACCESS_KEY_ID");
    c.secretAccessKey = qEnvironmentVariable("AWS_SECRET_ACCESS_KEY");
    c.sessionToken = qEnvironmentVariable("AWS_SESSION_TOKEN");
    if (!c.isValid()) {
        done(Error::make(ErrorCategory::Credentials,
                         QStringLiteral("AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY are not set")));
        return;
    }
    done(c);
}

AwsSharedFileCredentialsProvider::AwsSharedFileCredentialsProvider(QString path, QString profile)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path))
    , m_profile(std::move(profile))
{
    if (m_profile.isEmpty())
        m_profile = qEnvironmentVariable("AWS_PROFILE", QStringLiteral("default"));
}

QString AwsSharedFileCredentialsProvider::defaultPath()
{
    QString fromEnv = qEnvironmentVariable("AWS_SHARED_CREDENTIALS_FILE");
    if (!fromEnv.isEmpty())
        return fromEnv;
    return QDir::homePath() + QStringLiteral("/.aws/credentials");
}

Result<Credentials> AwsSharedFileCredentialsProvider::parse(const QByteArray& content,
                                                            const QString& profile)
{
    // Own parser (AwsIni.h): QSettings is off limits here, and its INI
    // escaping would mangle secrets containing '/', '+' or '\'.
    bool found = false;
    Credentials c;
    for (const detail::AwsIniSection& s : detail::parseAwsIni(content, false)) {
        if (s.name != profile)
            continue;
        found = true;
        const auto take = [&s](QString* field, const char* key) {
            if (s.values.contains(QByteArray(key)))
                *field = s.values.value(QByteArray(key));
        };
        take(&c.accessKeyId, "aws_access_key_id");
        take(&c.secretAccessKey, "aws_secret_access_key");
        take(&c.sessionToken, "aws_session_token");
    }
    if (!found) {
        return Error::make(ErrorCategory::Credentials,
                           QStringLiteral("profile [%1] not found in the credentials file").arg(profile));
    }
    if (!c.isValid()) {
        return Error::make(ErrorCategory::Credentials,
                           QStringLiteral("profile [%1] lacks aws_access_key_id / aws_secret_access_key")
                               .arg(profile));
    }
    return c;
}

void AwsSharedFileCredentialsProvider::fetch(Callback done)
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) {
        done(Error::make(ErrorCategory::Credentials,
                         QStringLiteral("cannot read %1: %2").arg(m_path, f.errorString())));
        return;
    }
    done(parse(f.readAll(), m_profile));
}

} // namespace qbucket::s3
