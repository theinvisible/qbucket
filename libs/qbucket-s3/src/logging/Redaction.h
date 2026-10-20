#pragma once

#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QString>
#include <QUrl>

// Secret redaction for anything that may reach a log, a RequestObserver or an
// Error: Authorization signatures, presigned-URL signatures, session tokens,
// SSE-C keys. Access key IDs are shortened, not removed (useful in support).
//
// Pure functions; thread-safe.
namespace qbucket::s3::redact {

inline constexpr char kRedacted[] = "<redacted>";

// "AKIAIOSFODNN7EXAMPLE" -> "AKIA…MPLE"; short IDs keep 2 characters.
QBUCKET_S3_EXPORT QString shortenAccessKeyId(const QString& id);

// Value of a request/response header, safe to log.
QBUCKET_S3_EXPORT QByteArray headerValue(const QByteArray& name, const QByteArray& value);

// A URL with X-Amz-Signature / X-Amz-Security-Token / X-Amz-Credential made
// safe; everything else untouched.
QBUCKET_S3_EXPORT QString url(const QUrl& url);

} // namespace qbucket::s3::redact
