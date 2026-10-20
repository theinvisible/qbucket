#pragma once

#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

// Any S3-compatible service reached through a user-entered endpoint: the
// plain S3 defaults, path-style addressing (works with every server, IP
// endpoints and dotted bucket names). Thread-safe (immutable).
class QBUCKET_S3_EXPORT GenericS3Profile : public ProviderProfile {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
};

} // namespace qbucket::s3
