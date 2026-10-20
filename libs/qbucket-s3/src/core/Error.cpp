#include <qbucket/s3/Error.h>

namespace qbucket::s3 {

QString Error::categoryName(ErrorCategory category)
{
    switch (category) {
    case ErrorCategory::Cancelled:          return QStringLiteral("Cancelled");
    case ErrorCategory::Transient:          return QStringLiteral("Transient");
    case ErrorCategory::Unsupported:        return QStringLiteral("Unsupported");
    case ErrorCategory::NotPermitted:       return QStringLiteral("NotPermitted");
    case ErrorCategory::Authentication:     return QStringLiteral("Authentication");
    case ErrorCategory::ClockSkew:          return QStringLiteral("ClockSkew");
    case ErrorCategory::NotFound:           return QStringLiteral("NotFound");
    case ErrorCategory::NotConfigured:      return QStringLiteral("NotConfigured");
    case ErrorCategory::PreconditionFailed: return QStringLiteral("PreconditionFailed");
    case ErrorCategory::Conflict:           return QStringLiteral("Conflict");
    case ErrorCategory::Redirect:           return QStringLiteral("Redirect");
    case ErrorCategory::InvalidRequest:     return QStringLiteral("InvalidRequest");
    case ErrorCategory::Protocol:           return QStringLiteral("Protocol");
    case ErrorCategory::Tls:                return QStringLiteral("Tls");
    case ErrorCategory::Credentials:        return QStringLiteral("Credentials");
    case ErrorCategory::Internal:           return QStringLiteral("Internal");
    case ErrorCategory::LocalIo:            return QStringLiteral("LocalIo");
    }
    return QStringLiteral("Unknown");
}

QString Error::toString() const
{
    QString out = categoryName(category);
    if (httpStatus != 0 || !code.isEmpty()) {
        out += QStringLiteral(" (");
        if (httpStatus != 0)
            out += QString::number(httpStatus);
        if (httpStatus != 0 && !code.isEmpty())
            out += QLatin1Char(' ');
        out += code;
        out += QLatin1Char(')');
    }
    if (!message.isEmpty())
        out += QStringLiteral(": ") + message;
    if (outcomeUnknown)
        out += QStringLiteral(" [outcome unknown]");
    return out;
}

Error Error::make(ErrorCategory category, const QString& message)
{
    Error e;
    e.category = category;
    e.message = message;
    return e;
}

} // namespace qbucket::s3
