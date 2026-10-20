#include <qbucket/s3/Endpoint.h>

namespace qbucket::s3 {

QString Endpoint::validationError() const
{
    if (!url.isValid() || url.host().isEmpty())
        return QStringLiteral("endpoint URL is missing or invalid");
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return QStringLiteral("endpoint URL must start with https:// or http://");
    if (!url.userInfo().isEmpty())
        return QStringLiteral("endpoint URL must not contain credentials");
    if (!url.path().isEmpty() && url.path() != QLatin1String("/"))
        return QStringLiteral("endpoint URL must not contain a path");
    if (url.hasQuery() || url.hasFragment())
        return QStringLiteral("endpoint URL must not contain a query or fragment");
    if (region.isEmpty())
        return QStringLiteral("region is missing");
    return {};
}

bool Endpoint::isHttps() const
{
    return url.scheme() == QLatin1String("https");
}

} // namespace qbucket::s3
