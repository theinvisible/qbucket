#pragma once

#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace qbucket::s3 {

// One profile of the AWS CLI's shared files, as a source to set an account
// up from. Holds the secret key: callers hand it to their secret store and
// never log the entry.
struct AwsProfileEntry {
    QString name;
    QString accessKeyId;
    QString secretAccessKey;
    QString sessionToken;               // temporary credentials
    QString region;
    // The S3 endpoint: the profile's nested `s3 = endpoint_url`, else its
    // [services] section's `s3 = endpoint_url`, else its `endpoint_url`.
    // Empty: AWS itself.
    QString endpointUrl;
    QString addressingStyle;            // `s3 = addressing_style`: auto | path | virtual; empty: unset
    // Keys that make the profile get its credentials elsewhere (sso_*,
    // credential_process, role_arn, source_profile, web_identity_token_file).
    QStringList indirectCredentials;
};

// The AWS CLI's shared configuration (~/.aws/credentials and ~/.aws/config),
// enumerated: what an import offers. Pure parsing, no I/O beyond the paths.
// Thread-safe (static, no shared state).
class QBUCKET_S3_EXPORT AwsConfigFiles {
public:
    // $AWS_SHARED_CREDENTIALS_FILE, else ~/.aws/credentials.
    [[nodiscard]] static QString credentialsPath();
    // $AWS_CONFIG_FILE, else ~/.aws/config.
    [[nodiscard]] static QString configPath();

    // Every profile of both files, merged by name: keys from either file
    // (the credentials file wins), the rest from the config file. In the
    // order first seen, the credentials file first. Comments (# ;), the
    // config file's "[profile x]" spelling, [services x] sections and
    // nested settings ("s3 =" followed by indented lines) are understood;
    // [sso-session x] and other sections are skipped.
    [[nodiscard]] static QList<AwsProfileEntry> parse(const QByteArray& credentials, const QByteArray& config);
};

} // namespace qbucket::s3
