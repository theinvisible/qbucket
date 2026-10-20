#pragma once

#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

// Garage (deuxfleurs.fr): self-hosted, deliberately small S3 subset.
// Path-style, default region "garage". Quirk: a wrong secret comes back as
// 403 AccessDenied "Forbidden: Invalid signature" — mapped to
// Authentication. Thread-safe (immutable).
class QBUCKET_S3_EXPORT GarageProfile : public ProviderProfile {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
    [[nodiscard]] QString signingRegion(const QString& region) const override;
    void mapError(Error& error) const override;
};

} // namespace qbucket::s3
