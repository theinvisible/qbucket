#pragma once

#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

// Amazon S3: regional endpoints https://s3.<region>.amazonaws.com,
// virtual-hosted addressing, region redirects resolved per bucket.
// Thread-safe (immutable).
class QBUCKET_S3_EXPORT AwsProfile : public ProviderProfile {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
    [[nodiscard]] QStringList regions() const override;
    [[nodiscard]] QUrl endpointFor(const QString& region) const override;
    [[nodiscard]] AddressingStyle defaultAddressing() const override;
    // Write-only features S3 itself defines: DeleteObjects, copies between
    // buckets.
    [[nodiscard]] CapabilitySet capabilities() const override;
    [[nodiscard]] QStringList storageClasses() const override;
};

} // namespace qbucket::s3
