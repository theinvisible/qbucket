#include <qbucket/s3/transfer/PartPlanner.h>

#include <algorithm>

namespace qbucket::s3::transfer {

namespace {

constexpr qint64 kMiB = Q_INT64_C(1024) * 1024;

Error invalid(const QString& message)
{
    return Error::make(ErrorCategory::InvalidRequest, message);
}

} // namespace

Result<PartPlan> planParts(qint64 size, const ProviderProfile& profile, const PartPlannerOptions& options)
{
    if (size < 0)
        return invalid(QStringLiteral("negative size"));
    if (size > kMaxObjectSize)
        return invalid(QStringLiteral("objects are limited to 5 TiB"));

    PartPlan plan;
    plan.size = size;
    // A single PUT carries at most one maximum part (5 GiB on S3).
    const qint64 threshold = std::min(options.multipartThreshold, profile.maxPartSize());
    if (size < threshold) {
        plan.partSize = size;
        plan.partCount = 1;
        return plan;
    }

    const int maxParts = std::max(1, profile.maxParts());
    qint64 part = std::max(options.minPartSize, profile.minPartSize());
    const qint64 needed = (size + maxParts - 1) / maxParts;   // ceil(size / maxParts)
    if (needed > part)
        part = (needed + kMiB - 1) / kMiB * kMiB;               // whole MiB
    if (part > profile.maxPartSize()) {
        return invalid(QStringLiteral("%1 bytes need parts above the server's maximum of %2 bytes")
                           .arg(size)
                           .arg(profile.maxPartSize()));
    }
    plan.multipart = true;
    plan.partSize = part;
    plan.partCount = int((size + part - 1) / part);
    if (plan.partCount > maxParts)
        return invalid(QStringLiteral("too many parts"));
    return plan;
}

} // namespace qbucket::s3::transfer
