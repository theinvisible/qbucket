#include <qbucket/s3/Credentials.h>

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

} // namespace qbucket::s3
