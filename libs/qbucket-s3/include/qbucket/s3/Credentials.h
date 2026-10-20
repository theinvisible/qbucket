#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Result.h>

#include <QDateTime>
#include <QString>

#include <functional>

namespace qbucket::s3 {

// One set of access credentials. Value type; reentrant.
//
// Holds the secret in memory — never log it; Error / RequestRecord never
// contain it. Consumers that persist credentials must use a secret store.
struct QBUCKET_S3_EXPORT Credentials {
    QString accessKeyId;
    QString secretAccessKey;
    QString sessionToken;   // temporary credentials only
    QDateTime expiresAt;    // invalid: does not expire

    [[nodiscard]] bool isValid() const
    {
        return !accessKeyId.isEmpty() && !secretAccessKey.isEmpty();
    }

    // True when the credentials expire within `marginSecs` of `now`.
    [[nodiscard]] bool isExpired(const QDateTime& now, int marginSecs = 60) const
    {
        return expiresAt.isValid() && now.addSecs(marginSecs) >= expiresAt;
    }
};

// Where a Session gets its credentials from — the inversion point for
// keychains, environment variables, credential files, STS, …
//
// Thread-safety: implementations must be thread-safe. fetch() is called on
// the library's worker threads.
class QBUCKET_S3_EXPORT CredentialsProvider {
public:
    using Callback = std::function<void(Result<Credentials>)>;

    virtual ~CredentialsProvider();

    // Delivers credentials (or an Error with category Credentials) by
    // invoking `done` exactly once — synchronously or later, from any thread.
    // The library re-marshals the result onto its own thread.
    virtual void fetch(Callback done) = 0;

    // The server rejected the last credentials (InvalidAccessKeyId,
    // ExpiredToken, …): drop caches so the next fetch() reloads.
    virtual void invalidate() {}

protected:
    CredentialsProvider() = default;
    CredentialsProvider(const CredentialsProvider&) = default;
    CredentialsProvider& operator=(const CredentialsProvider&) = default;
};

// Fixed credentials held in memory. Thread-safe (immutable).
class QBUCKET_S3_EXPORT StaticCredentialsProvider final : public CredentialsProvider {
public:
    explicit StaticCredentialsProvider(Credentials credentials);
    void fetch(Callback done) override;

private:
    Credentials m_credentials;
};

// AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN, read on
// every fetch(). Thread-safe.
class QBUCKET_S3_EXPORT EnvCredentialsProvider final : public CredentialsProvider {
public:
    void fetch(Callback done) override;
};

// The AWS shared credentials file (~/.aws/credentials format): sections
// [profile] with aws_access_key_id, aws_secret_access_key and optional
// aws_session_token. Read on every fetch(), so edits apply without a
// restart. Thread-safe (immutable configuration, no shared state).
class QBUCKET_S3_EXPORT AwsSharedFileCredentialsProvider final : public CredentialsProvider {
public:
    // Empty path: defaultPath(). Empty profile: $AWS_PROFILE, else "default".
    explicit AwsSharedFileCredentialsProvider(QString path = {}, QString profile = {});
    void fetch(Callback done) override;

    // $AWS_SHARED_CREDENTIALS_FILE, else ~/.aws/credentials.
    [[nodiscard]] static QString defaultPath();

    // The parser on its own (also used by tests).
    [[nodiscard]] static Result<Credentials> parse(const QByteArray& content, const QString& profile);

private:
    QString m_path;
    QString m_profile;
};

} // namespace qbucket::s3
