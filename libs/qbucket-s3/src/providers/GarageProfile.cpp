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

QStringList GarageProfile::storageClasses() const
{
    return {QStringLiteral("STANDARD")};
}

CapabilitySet GarageProfile::capabilities() const
{
    // Write-only features the probes can't see (garagehq.deuxfleurs.fr,
    // "S3 compatibility"): no versioning, no object lock, no SSE-C;
    // DeleteObjects is implemented.
    CapabilitySet set = ProviderProfile::capabilities();
    const CapabilityEntry no{CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0};
    set.set(Feature::BatchDelete, {CapabilityState::Supported, CapabilitySource::Profile, {}, 0, {}, false, 0});
    set.set(Feature::BucketVersioning, no);
    set.set(Feature::BucketVersioningWrite, no);   // "Unimplemented action: PutBucketVersioning"
    set.set(Feature::ObjectLock, no);
    set.set(Feature::SseC, no);
    // If-None-Match: * is ignored and the write overwrites (Garage 1.x in
    // tests/docker, it_s3_backend conditionalPutBehaviour).
    set.set(Feature::ConditionalWrites, no);
    // If-Match on PutObject / Complete too (it_s3_backend conditionalOverwrite).
    set.set(Feature::ConditionalOverwrite, no);
    // No object tags and no object ACLs (the read probes answer 501 too).
    set.set(Feature::ObjectTaggingWrite, no);
    set.set(Feature::ObjectAclWrite, no);
    // Bucket configurations (it_s3_backend_garage bucketConfigWriteAndRemove):
    // CORS and lifecycle are written, read back and removed; no policy, no
    // public access block, no ACL writes (501), no object lock.
    const CapabilityEntry yes{CapabilityState::Supported, CapabilitySource::Profile, {}, 0, {}, false, 0};
    set.set(Feature::CorsWrite, yes);
    set.set(Feature::LifecycleWrite, yes);
    for (const Feature f : {Feature::BucketPolicyWrite, Feature::PublicAccessBlockWrite, Feature::BucketAclWrite,
                            Feature::ObjectLockWrite, Feature::RetentionWrite, Feature::LegalHoldWrite}) {
        set.set(f, no);
    }
    return set;
}

void GarageProfile::reviewProbe(Feature feature, CapabilityEntry& entry) const
{
    // GET ?versioning answers an empty configuration, PUT ?versioning is 501
    // (tests/data/garage/put-versioning-not-implemented.xml).
    if (feature == Feature::BucketVersioning && entry.state == CapabilityState::Supported) {
        entry.state = CapabilityState::Unsupported;
        entry.s3Code = QStringLiteral("ProfileOverride:StubGetBucketVersioning");
    }
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
