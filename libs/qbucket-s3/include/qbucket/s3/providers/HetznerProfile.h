#pragma once

#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

// Hetzner Object Storage (Ceph RGW underneath) — QBucket's first-class
// provider (PLAN.md §3.2). Endpoints https://<location>.your-objectstorage.com,
// signing region = location, virtual-hosted addressing.
//
// Capabilities and quirks are established by probing a real project
// (docs/providers/hetzner.md). Quirks (tests/data/hetzner):
// - valid requests sporadically get 403 AccessDenied with an empty
//   message — marked retryable;
// - a front proxy handles writes to subresources Hetzner doesn't support as
//   plain bucket operations: DELETE /?encryption ran as DeleteBucket. Read
//   probes reach RGW and look fine, so such features are overridden to
//   Unsupported (reviewProbe) and the library refuses them locally.
// Thread-safe (immutable).
class QBUCKET_S3_EXPORT HetznerProfile : public ProviderProfile {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
    [[nodiscard]] QStringList regions() const override;
    [[nodiscard]] QUrl endpointFor(const QString& region) const override;
    [[nodiscard]] AddressingStyle defaultAddressing() const override;
    [[nodiscard]] QString signingRegion(const QString& region) const override;
    [[nodiscard]] QString regionFromLocationConstraint(const QString& constraint) const override;
    [[nodiscard]] QString locationConstraintFor(const QString& region) const override;
    [[nodiscard]] CapabilitySet capabilities() const override;
    [[nodiscard]] QStringList storageClasses() const override;
    void reviewProbe(Feature feature, CapabilityEntry& entry) const override;
    void mapError(Error& error) const override;
};

} // namespace qbucket::s3
