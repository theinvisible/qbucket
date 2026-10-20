#include <qbucket/s3/Credentials.h>

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
    const QString fromEnv = qEnvironmentVariable("AWS_SHARED_CREDENTIALS_FILE");
    if (!fromEnv.isEmpty())
        return fromEnv;
    return QDir::homePath() + QStringLiteral("/.aws/credentials");
}

Result<Credentials> AwsSharedFileCredentialsProvider::parse(const QByteArray& content,
                                                            const QString& profile)
{
    // Own parser: QSettings is off limits here, and its INI escaping would
    // mangle secrets containing '/', '+' or '\'.
    QString section;
    bool found = false;
    Credentials c;
    for (QByteArray line : content.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            section = QString::fromUtf8(line.mid(1, line.size() - 2)).trimmed();
            if (section.startsWith(QLatin1String("profile ")))   // ~/.aws/config spelling
                section = section.mid(8).trimmed();
            found = found || section == profile;
            continue;
        }
        if (section != profile)
            continue;
        const qsizetype eq = line.indexOf('=');
        if (eq < 0)
            continue;
        const QByteArray name = line.left(eq).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(eq + 1).trimmed());
        if (name == "aws_access_key_id")
            c.accessKeyId = value;
        else if (name == "aws_secret_access_key")
            c.secretAccessKey = value;
        else if (name == "aws_session_token")
            c.sessionToken = value;
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
