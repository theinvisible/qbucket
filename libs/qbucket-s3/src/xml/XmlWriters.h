#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>

#include <QByteArray>
#include <QList>

// Request bodies. Pure functions; thread-safe.
namespace qbucket::s3::xml {

// <Delete> body for DeleteObjects. Fails with InvalidRequest when a key holds
// characters XML 1.0 cannot carry (control characters other than tab, LF,
// CR) — such objects must be deleted one by one.
QBUCKET_S3_EXPORT Result<QByteArray> buildDeleteRequest(const QList<ObjectIdentifier>& objects,
                                                        bool quiet);

// <CreateBucketConfiguration> with a LocationConstraint.
QBUCKET_S3_EXPORT QByteArray buildCreateBucketConfiguration(const QString& locationConstraint);

// Escapes text for element content; CR, LF and tab become character
// references so the server's XML parser cannot normalise them away.
QBUCKET_S3_EXPORT QByteArray escapeText(const QString& text);

} // namespace qbucket::s3::xml
