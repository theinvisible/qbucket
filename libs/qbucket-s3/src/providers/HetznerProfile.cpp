#include <qbucket/s3/providers/HetznerProfile.h>

namespace qbucket::s3 {

QString HetznerProfile::id() const
{
    return QStringLiteral("hetzner");
}

QString HetznerProfile::displayName() const
{
    return QStringLiteral("Hetzner Object Storage");
}

QStringList HetznerProfile::regions() const
{
    // docs.hetzner.com/storage/object-storage/overview (checked 2026-10-20).
    return {QStringLiteral("fsn1"), QStringLiteral("nbg1"), QStringLiteral("hel1")};
}

QUrl HetznerProfile::endpointFor(const QString& region) const
{
    if (region.isEmpty())
        return {};
    return QUrl(QStringLiteral("https://%1.your-objectstorage.com").arg(region));
}

AddressingStyle HetznerProfile::defaultAddressing() const
{
    return AddressingStyle::VirtualHosted;
}

QString HetznerProfile::signingRegion(const QString& region) const
{
    // The location is the signing region; there is no us-east-1 fallback.
    return region;
}

QString HetznerProfile::regionFromLocationConstraint(const QString& constraint) const
{
    return constraint;
}

QString HetznerProfile::locationConstraintFor(const QString& region) const
{
    return region;
}

} // namespace qbucket::s3
