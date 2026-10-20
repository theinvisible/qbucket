#include <qbucket/s3/providers/GarageProfile.h>

namespace qbucket::s3 {

QString GarageProfile::id() const
{
    return QStringLiteral("garage");
}

QString GarageProfile::displayName() const
{
    return QStringLiteral("Garage");
}

QString GarageProfile::signingRegion(const QString& region) const
{
    return region.isEmpty() ? QStringLiteral("garage") : region;
}

void GarageProfile::mapError(Error& error) const
{
    // tests/data/garage/error-invalid-signature.xml
    if (error.category == ErrorCategory::NotPermitted && error.code == QLatin1String("AccessDenied")
        && error.message.contains(QLatin1String("Invalid signature"), Qt::CaseInsensitive)) {
        error.category = ErrorCategory::Authentication;
    }
}

} // namespace qbucket::s3
