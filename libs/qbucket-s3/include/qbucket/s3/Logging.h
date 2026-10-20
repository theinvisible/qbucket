#pragma once

#include <qbucket/s3/Export.h>

#include <QLoggingCategory>

// Logging categories of qbucket-s3. Filter them like any Qt category, e.g.
// QT_LOGGING_RULES="qbucket.s3.http.debug=true".
//
// Secrets never reach these categories: Authorization signatures, presigned
// URL signatures, session tokens and SSE-C keys are redacted before logging.
namespace qbucket::s3 {

Q_DECLARE_EXPORTED_LOGGING_CATEGORY(lcHttp, QBUCKET_S3_EXPORT)          // qbucket.s3.http
Q_DECLARE_EXPORTED_LOGGING_CATEGORY(lcSigning, QBUCKET_S3_EXPORT)       // qbucket.s3.signing
Q_DECLARE_EXPORTED_LOGGING_CATEGORY(lcSession, QBUCKET_S3_EXPORT)       // qbucket.s3.session
Q_DECLARE_EXPORTED_LOGGING_CATEGORY(lcRetry, QBUCKET_S3_EXPORT)         // qbucket.s3.retry
Q_DECLARE_EXPORTED_LOGGING_CATEGORY(lcCapabilities, QBUCKET_S3_EXPORT)  // qbucket.s3.capabilities

} // namespace qbucket::s3
