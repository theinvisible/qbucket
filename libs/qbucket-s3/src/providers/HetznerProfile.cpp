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

// Hetzner documents no storage classes; RGW's default placement has only
// STANDARD (it_s3_backend copyInPlaceClassChange shows what another class does).
QStringList HetznerProfile::storageClasses() const
{
    return {QStringLiteral("STANDARD")};
}

// Hetzner's documentation names only these (for buckets); objects the same.
QStringList HetznerProfile::cannedObjectAcls() const
{
    return {QStringLiteral("private"), QStringLiteral("public-read")};
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
    // Hetzner's "supported actions" page says CopyObject works within one
    // bucket only; a copy qbucket-it-test → qbucket-it-lock (nbg1, same
    // project) worked and read back intact (devlog 005, it_s3_backend
    // crossBucketCopy). Safe either way: a refused copy is an error and the
    // app never deletes a source whose copy failed.
    known(Feature::CrossBucketCopy, CapabilityState::Supported);
    known(Feature::SseC, CapabilityState::Supported);                // the only encryption mode
    known(Feature::BucketEncryption, CapabilityState::Unsupported);  // no SSE-S3 / SSE-KMS
    known(Feature::BatchDelete, CapabilityState::Supported);
    // If-None-Match: * on PutObject is ignored — the object was overwritten
    // (nbg1, 2026-10-20, it_s3_backend conditionalPutBehaviour). Refused
    // locally, so nobody relies on it; uploads check with HEAD instead.
    // Multipart requests do keep their query string: UploadPart and Abort
    // on the key of an existing object left it intact
    // (multipartRequestsLeaveAnExistingObjectAlone).
    known(Feature::ConditionalWrites, CapabilityState::Unsupported);
    // If-Match on PutObject refuses even the right (quoted) ETag, and
    // CompleteMultipartUpload ignores it — like Ceph RGW 19.2 (nbg1,
    // 2026-10-21, it_s3_backend conditionalOverwrite). Replacing an object
    // checks with HEAD right before instead.
    known(Feature::ConditionalOverwrite, CapabilityState::Unsupported);
    // Object tag and ACL writes keep their subresource through the proxy:
    // PUT ?tagging / ?acl on a missing key answer 404 and create nothing, on
    // an existing object they leave content, ETag and size alone and read
    // back (qbucket-it-test and qbucket-it-lock, nbg1, 2026-10-21;
    // it_s3_backend subresourceWritesOnMissingKeyCreateNothing,
    // objectSubresourceWritesLeaveDataAlone). Not DELETE ?tagging — the
    // library never sends it.
    known(Feature::ObjectTaggingWrite, CapabilityState::Supported);
    known(Feature::ObjectAclWrite, CapabilityState::Supported);
    // PUT ?versioning keeps its subresource too: on a missing bucket it
    // answers 404 NoSuchBucket and creates nothing; on the scratch bucket
    // qbucket-it-versioning (guard object) Enabled → Suspended → Enabled
    // read back at once, the guard intact (nbg1, 2026-10-21; it_s3_backend
    // bucketVersioningWriteOnMissingBucketCreatesNothing, toggleVersioning).
    known(Feature::BucketVersioningWrite, CapabilityState::Supported);
    // Bucket configurations reach RGW as well: PUT ?policy / ?cors /
    // ?lifecycle / ?publicAccessBlock / ?acl / ?object-lock on a missing
    // bucket answer 404 and create nothing; on qbucket-it-versioning (guard
    // object) each was written, read back at once and removed (DELETE, or
    // the ACL back to private), bucket and guard intact. Default retention,
    // retention and legal hold work on qbucket-it-lockcfg (GOVERNANCE only),
    // the bypass included. (nbg1, 2026-10-21; it_s3_backend
    // bucketConfigWritesOnMissingBucketCreateNothing,
    // bucketConfigWriteAndRemove, objectLockAndRetention.)
    for (const Feature f : {Feature::BucketPolicyWrite, Feature::CorsWrite, Feature::LifecycleWrite,
                            Feature::ObjectLockWrite, Feature::BucketAclWrite, Feature::PublicAccessBlockWrite,
                            Feature::RetentionWrite, Feature::LegalHoldWrite}) {
        known(f, CapabilityState::Supported);
    }
    return set;
}

int HetznerProfile::bucketEmptinessSettleSecs() const
{
    // The proxy answers DeleteBucket itself and judges "empty" by statistics
    // that lag: within ~7–10 s of a write into a bucket that looked empty,
    // DeleteBucket answers 204 and the bucket is gone with the new objects;
    // from ~12 s on 409 BucketNotEmpty (nbg1, 2026-10-21, it_s3_backend
    // deleteBucketRightAfterAWrite; docs/providers/hetzner.md).
    return 20;
}

void HetznerProfile::reviewProbe(Feature feature, CapabilityEntry& entry) const
{
    // GET ?encryption reaches RGW (404 …NotFoundError: "supported, not set"),
    // but PUT/DELETE ?encryption are intercepted by Hetzner's proxy — DELETE
    // was executed as DeleteBucket (tests/data/hetzner/README.md). The read
    // answer must not unlock the write.
    if (feature == Feature::BucketEncryption && entry.state == CapabilityState::Supported) {
        entry.state = CapabilityState::Unsupported;
        entry.s3Code = QStringLiteral("ProfileOverride:WritesInterceptedByProxy");
    }
}

void HetznerProfile::mapError(Error& error) const
{
    // Observed 2026-10-20 on nbg1: 10–30 % of identical, valid requests
    // answered 403 AccessDenied with an empty <Message> (or, for HEAD, a
    // bare 403). Retrying is safe for idempotent requests; a genuine denial
    // simply fails again and keeps its NotPermitted category.
    if (error.httpStatus == 403 && error.category == ErrorCategory::NotPermitted
        && (error.code.isEmpty() || error.code == QLatin1String("AccessDenied"))
        && (error.message.isEmpty() || error.message == QLatin1String("HTTP 403"))) {
        error.retryable = true;
    }
    // Observed 2026-10-21 on nbg1: for a bucket created an hour before, one
    // RGW gateway (of ~20) still answered every DELETE with 404 NoSuchBucket
    // and an empty <Message> (tests/data/hetzner); the proxy's own answer for
    // a bucket that doesn't exist carries a message. Another attempt reaches
    // another gateway; a bucket that is really gone fails again.
    if (error.httpStatus == 404 && error.code == QLatin1String("NoSuchBucket")
        && (error.message.isEmpty() || error.message == QLatin1String("HTTP 404")))
        error.retryable = true;
}

} // namespace qbucket::s3
