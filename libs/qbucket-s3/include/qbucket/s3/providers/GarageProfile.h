#pragma once

#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

// Garage (deuxfleurs.fr): self-hosted, deliberately small S3 subset.
// Path-style, default region "garage". Quirks (fixtures in tests/data/garage):
// a wrong secret comes back as 403 AccessDenied "Forbidden: Invalid
// signature" (→ Authentication); GET ?versioning is a stub answering an
// empty configuration although versioning cannot be enabled (→ Unsupported).
// Thread-safe (immutable).
class QBUCKET_S3_EXPORT GarageProfile : public ProviderProfile {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
    [[nodiscard]] QString signingRegion(const QString& region) const override;
    [[nodiscard]] CapabilitySet capabilities() const override;
    void reviewProbe(Feature feature, CapabilityEntry& entry) const override;
    void mapError(Error& error) const override;
};

} // namespace qbucket::s3
