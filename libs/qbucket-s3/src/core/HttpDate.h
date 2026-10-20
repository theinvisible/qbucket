#pragma once

#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QDateTime>

namespace qbucket::s3 {

// HTTP dates (RFC 7231 IMF-fixdate, "Sun, 06 Nov 1994 08:49:37 GMT") as used
// in Date / Last-Modified headers. Invalid QDateTime on parse failure.
QBUCKET_S3_EXPORT QDateTime parseHttpDate(const QByteArray& value);
QBUCKET_S3_EXPORT QByteArray formatHttpDate(const QDateTime& time);

} // namespace qbucket::s3
