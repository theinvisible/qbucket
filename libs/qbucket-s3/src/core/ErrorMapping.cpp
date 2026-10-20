#include "core/ErrorMapping.h"

#include "xml/XmlParsers.h"

#include <QHash>

namespace qbucket::s3 {

ErrorCategory categoryForCode(const QString& code)
{
    static const QHash<QString, ErrorCategory> table = [] {
        QHash<QString, ErrorCategory> t;
        const auto add = [&t](std::initializer_list<const char*> codes, ErrorCategory c) {
            for (const char* code : codes)
                t.insert(QString::fromLatin1(code), c);
        };
        add({"AccessDenied", "AllAccessDisabled", "AccountProblem", "InvalidObjectState"},
            ErrorCategory::NotPermitted);
        add({"InvalidAccessKeyId", "SignatureDoesNotMatch", "ExpiredToken", "InvalidToken",
             "TokenRefreshRequired", "InvalidSecurity", "MissingSecurityHeader"},
            ErrorCategory::Authentication);
        add({"RequestTimeTooSkewed"}, ErrorCategory::ClockSkew);
        add({"NoSuchBucket", "NoSuchKey", "NoSuchVersion", "NoSuchUpload"},
            ErrorCategory::NotFound);
        add({"NoSuchBucketPolicy", "NoSuchCORSConfiguration", "NoSuchLifecycleConfiguration",
             "NoSuchTagSet", "NoSuchTagSetError", "NoSuchWebsiteConfiguration",
             "ObjectLockConfigurationNotFoundError", "NoSuchObjectLockConfiguration",
             "ServerSideEncryptionConfigurationNotFoundError",
             "ReplicationConfigurationNotFoundError", "NoSuchPublicAccessBlockConfiguration",
             "OwnershipControlsNotFoundError", "NoSuchNotificationConfiguration",
             "NoSuchLoggingConfiguration"},
            ErrorCategory::NotConfigured);
        add({"NotImplemented", "XNotImplemented"}, ErrorCategory::Unsupported);
        add({"SlowDown", "ServiceUnavailable", "InternalError", "RequestTimeout",
             "OperationTimedOut", "ServiceFailure"},
            ErrorCategory::Transient);
        add({"PreconditionFailed"}, ErrorCategory::PreconditionFailed);
        add({"BucketNotEmpty", "BucketAlreadyExists", "BucketAlreadyOwnedByYou",
             "OperationAborted", "InvalidBucketState"},
            ErrorCategory::Conflict);
        add({"PermanentRedirect", "TemporaryRedirect", "AuthorizationHeaderMalformed",
             "IllegalLocationConstraintException"},
            ErrorCategory::Redirect);
        return t;
    }();
    return table.value(code, ErrorCategory::Internal);
}

ErrorCategory categoryForStatus(int httpStatus)
{
    switch (httpStatus) {
    case 301:
    case 307:
        return ErrorCategory::Redirect;
    case 403:
        return ErrorCategory::NotPermitted;
    case 404:
        return ErrorCategory::NotFound;
    case 409:
        return ErrorCategory::Conflict;
    case 412:
        return ErrorCategory::PreconditionFailed;
    case 501:
        return ErrorCategory::Unsupported;
    default:
        break;
    }
    if (httpStatus >= 500)
        return ErrorCategory::Transient;
    if (httpStatus >= 400)
        return ErrorCategory::InvalidRequest;
    return ErrorCategory::Protocol;
}

Error errorFromResponse(int httpStatus, const QByteArray& body, const QString& requestIdHeader,
                        const QString& hostIdHeader)
{
    Error e;
    e.httpStatus = httpStatus;
    e.requestId = requestIdHeader;
    e.hostId = hostIdHeader;

    if (const auto parsed = xml::parseErrorBody(body)) {
        e.code = parsed->code;
        e.message = parsed->message;
        e.resource = parsed->resource;
        if (!parsed->requestId.isEmpty())
            e.requestId = parsed->requestId;
        if (!parsed->hostId.isEmpty())
            e.hostId = parsed->hostId;
    }

    e.category = e.code.isEmpty() ? ErrorCategory::Internal : categoryForCode(e.code);
    if (e.category == ErrorCategory::Internal)   // unknown code or no body
        e.category = categoryForStatus(httpStatus);
    if (e.message.isEmpty())
        e.message = QStringLiteral("HTTP %1").arg(httpStatus);
    return e;
}

} // namespace qbucket::s3
