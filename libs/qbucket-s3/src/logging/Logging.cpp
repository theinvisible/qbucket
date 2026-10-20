#include <qbucket/s3/Logging.h>

namespace qbucket::s3 {

Q_LOGGING_CATEGORY(lcHttp, "qbucket.s3.http", QtWarningMsg)
Q_LOGGING_CATEGORY(lcSigning, "qbucket.s3.signing", QtWarningMsg)
Q_LOGGING_CATEGORY(lcSession, "qbucket.s3.session", QtWarningMsg)
Q_LOGGING_CATEGORY(lcRetry, "qbucket.s3.retry", QtWarningMsg)
Q_LOGGING_CATEGORY(lcCapabilities, "qbucket.s3.capabilities", QtWarningMsg)

} // namespace qbucket::s3
