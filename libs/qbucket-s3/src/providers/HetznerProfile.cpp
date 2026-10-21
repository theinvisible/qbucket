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
    return set;
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
}

} // namespace qbucket::s3
