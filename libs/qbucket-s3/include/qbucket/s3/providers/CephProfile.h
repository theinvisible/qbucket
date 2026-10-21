#pragma once

#include <qbucket/s3/providers/ProviderProfile.h>

namespace qbucket::s3 {

// Ceph RGW, self-hosted (Squid in tests/docker; Hetzner runs RGW too, behind
// its own proxy — that one has HetznerProfile). Path-style by default, the
// endpoint is the user's. What the read probes can't see comes from
// it_s3_backend_ceph (tests/data/it-expectations/ceph.json): conditional
// writes, tag / ACL writes, versioning, every bucket configuration this
// library writes, retention and legal hold. RGW ignores ?ownershipControls
// and answers the bucket listing (tests/data/ceph) — the probe reads that
// as Unsupported.
// Thread-safe (immutable).
class QBUCKET_S3_EXPORT CephProfile : public ProviderProfile {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
    [[nodiscard]] CapabilitySet capabilities() const override;
    [[nodiscard]] QStringList storageClasses() const override;
};

} // namespace qbucket::s3
