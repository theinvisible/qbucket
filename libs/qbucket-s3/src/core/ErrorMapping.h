#pragma once

#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QString>

namespace qbucket::s3 {

// Generic S3 mapping of an HTTP error response to an Error: category from the
// S3 code where there is one, else from the status. Provider profiles refine
// the result afterwards (ProviderProfile::mapError).
//
// 405 MethodNotAllowed maps to InvalidRequest here — e.g. GET on a delete
// marker. Only the capability prober treats 405 as "unsupported".
//
// Pure; thread-safe.
QBUCKET_S3_EXPORT ErrorCategory categoryForCode(const QString& code);
QBUCKET_S3_EXPORT ErrorCategory categoryForStatus(int httpStatus);

// Full error from status + (possibly empty, e.g. HEAD) body.
QBUCKET_S3_EXPORT Error errorFromResponse(int httpStatus, const QByteArray& body,
                                          const QString& requestIdHeader = {},
                                          const QString& hostIdHeader = {});

} // namespace qbucket::s3
