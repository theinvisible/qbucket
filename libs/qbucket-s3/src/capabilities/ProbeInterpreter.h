#pragma once

#include "client/RequestPipeline.h"

#include <qbucket/s3/Capabilities.h>
#include <qbucket/s3/Export.h>

#include <QList>
#include <QStringList>

namespace qbucket::s3 {

// One read-only probe: GET ?<subresource> on the bucket (or on one object).
struct ProbeSpec {
    Feature feature;
    QueryParams query;              // e.g. {"versioning", bare}
    QLatin1StringView expectedRoot; // empty: JSON object (bucket policy)
    bool objectLevel = false;
    QStringList notConfiguredCodes; // 404 codes meaning "supported, nothing set"
};

QBUCKET_S3_EXPORT const QList<ProbeSpec>& bucketProbes();
QBUCKET_S3_EXPORT const QList<ProbeSpec>& objectProbes();

// The response interpretation table (PLAN.md §3.3), pure:
//   200 + expected root               → Supported (configured if something is set)
//   404 "not configured" codes        → Supported, not configured
//   501 / 405 / profile-mapped        → Unsupported
//   200 + another root (e.g. a listing: the subresource was ignored) → Unsupported
//   403                               → NotPermitted
//   5xx, timeout, network, other      → Unknown — never Unsupported
// Updates *state for versioning / object lock when given.
QBUCKET_S3_EXPORT CapabilityEntry interpretProbe(const ProbeSpec& spec,
                                                 const Result<RawResponse>& response,
                                                 const QDateTime& now, BucketState* state = nullptr);

// Fingerprint of the server from a response's headers.
QBUCKET_S3_EXPORT ServerFingerprint fingerprintOf(const RawResponse& response);

} // namespace qbucket::s3
