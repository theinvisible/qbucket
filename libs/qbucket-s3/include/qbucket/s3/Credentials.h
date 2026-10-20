#pragma once

#include <qbucket/s3/Export.h>

#include <QDateTime>
#include <QString>

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

} // namespace qbucket::s3
