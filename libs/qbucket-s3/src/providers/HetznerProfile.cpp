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

CapabilitySet HetznerProfile::capabilities() const
{
    // Write-only features the probes can't see. Source: Hetzner's
    // "supported actions" page (2026-10-20) — preliminary until confirmed
    // against a real project (docs/providers/hetzner.md).
    CapabilitySet set = ProviderProfile::capabilities();
    const auto known = [&set](Feature f, CapabilityState s) {
        set.set(f, {s, CapabilitySource::Profile, {}, 0, {}, false, 0});
    };
    known(Feature::CrossBucketCopy, CapabilityState::Unsupported);   // CopyObject within one bucket only
    known(Feature::SseC, CapabilityState::Supported);                // the only encryption mode
    known(Feature::BucketEncryption, CapabilityState::Unsupported);  // no SSE-S3 / SSE-KMS
    known(Feature::BatchDelete, CapabilityState::Supported);
    return set;
}

} // namespace qbucket::s3
