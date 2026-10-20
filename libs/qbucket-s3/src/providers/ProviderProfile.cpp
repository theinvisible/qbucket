#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

ProviderProfile::~ProviderProfile() = default;

QUrl ProviderProfile::endpointFor(const QString& /*region*/) const
{
    return {};
}

QString ProviderProfile::signingRegion(const QString& region) const
{
    return region.isEmpty() ? QStringLiteral("us-east-1") : region;
}

QString ProviderProfile::regionFromLocationConstraint(const QString& constraint) const
{
    if (constraint.isEmpty())
        return QStringLiteral("us-east-1");
    if (constraint == QLatin1String("EU"))
        return QStringLiteral("eu-west-1");
    return constraint;
}

QString ProviderProfile::locationConstraintFor(const QString& region) const
{
    return region == QLatin1String("us-east-1") ? QString() : region;
}

void ProviderProfile::prepareRequest(RequestContext& /*ctx*/) const {}

void ProviderProfile::normalizeListing(ListPage& page) const
{
    for (ObjectEntry& e : page.objects) {
        if (!e.etag.isEmpty() && !e.etag.startsWith(QLatin1Char('"')))
            e.etag = QLatin1Char('"') + e.etag + QLatin1Char('"');
    }
}

void ProviderProfile::mapError(Error& /*error*/) const {}

QString GenericS3Profile::id() const
{
    return QStringLiteral("generic");
}

QString GenericS3Profile::displayName() const
{
    return QStringLiteral("S3-compatible (custom endpoint)");
}

} // namespace qbucket::s3
