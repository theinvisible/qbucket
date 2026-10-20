#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/providers/ProviderProfile.h>
#include <qbucket/s3/transfer/Export.h>

#include <QtGlobal>

namespace qbucket::s3::transfer {

// How a transfer of `size` bytes is split (PLAN.md §6 M3). Value type.
struct PartPlan {
    qint64 size = 0;
    bool multipart = false;   // false: one PUT / one GET
    qint64 partSize = 0;      // all parts but the last; == size for a single request
    int partCount = 1;

    // 1-based part numbers.
    [[nodiscard]] qint64 partOffset(int partNumber) const { return qint64(partNumber - 1) * partSize; }
    [[nodiscard]] qint64 partLength(int partNumber) const
    {
        return partNumber < partCount ? partSize : size - partOffset(partNumber);
    }
};

struct PartPlannerOptions {
    // From this size on a transfer is split.
    qint64 multipartThreshold = Q_INT64_C(64) * 1024 * 1024;
    // The smallest part we use (never below the provider's minimum).
    qint64 minPartSize = Q_INT64_C(8) * 1024 * 1024;
};

// The largest object S3 stores.
inline constexpr qint64 kMaxObjectSize = Q_INT64_C(5) * 1024 * 1024 * 1024 * 1024;

// Pure: part size max(minPartSize, size / maxParts rounded up to whole
// MiB), within the profile's limits (min/max part size, max parts). Fails
// with InvalidRequest for negative sizes and objects above 5 TiB or the
// profile's limits.
QBUCKET_S3_TRANSFER_EXPORT Result<PartPlan> planParts(qint64 size, const ProviderProfile& profile,
                                                      const PartPlannerOptions& options = {});

} // namespace qbucket::s3::transfer
