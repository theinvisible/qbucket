#include <qbucket/s3/providers/CephProfile.h>

namespace qbucket::s3 {

QString CephProfile::id() const
{
    return QStringLiteral("ceph");
}

QString CephProfile::displayName() const
{
    return QStringLiteral("Ceph RGW (self-hosted)");
}

QStringList CephProfile::storageClasses() const
{
    // Placement targets can add more; STANDARD is the one every zone has.
    return {QStringLiteral("STANDARD")};
}

CapabilitySet CephProfile::capabilities() const
{
    // Confirmed against Ceph Squid by it_s3_backend_ceph (2026-10-21).
    CapabilitySet set = ProviderProfile::capabilities();
    const CapabilityEntry yes{CapabilityState::Supported, CapabilitySource::Profile, {}, 0, {}, false, 0};
    for (const Feature f : {Feature::BatchDelete, Feature::CrossBucketCopy, Feature::ConditionalWrites,
                            Feature::ObjectTaggingWrite, Feature::ObjectAclWrite, Feature::BucketVersioningWrite,
                            Feature::BucketPolicyWrite, Feature::CorsWrite, Feature::LifecycleWrite,
                            Feature::ObjectLockWrite, Feature::BucketAclWrite, Feature::PublicAccessBlockWrite,
                            Feature::RetentionWrite, Feature::LegalHoldWrite}) {
        set.set(f, yes);
    }
    return set;
}

} // namespace qbucket::s3
