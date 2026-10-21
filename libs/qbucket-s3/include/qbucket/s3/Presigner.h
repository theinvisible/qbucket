#pragma once

#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Result.h>

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace qbucket::s3 {

struct PresignRequest {
    QByteArray method = "GET";
    QString bucket;
    QString key;
    QString versionId;       // empty: current version
    int expiresSeconds = 3600;
    // GET only: response-content-disposition, e.g. `attachment; filename="a.pdf"`
    // — the browser saves instead of showing. Empty: the object's own.
    QString responseContentDisposition;
};

// Presigned URLs without a network round-trip (SigV4 query signing,
// UNSIGNED-PAYLOAD). Thread-safe: only static, pure functions.
class QBUCKET_S3_EXPORT Presigner {
public:
    // SigV4 allows at most 7 days; a provider profile may allow less.
    static constexpr int kMaxExpirySeconds = 7 * 24 * 3600;

    // `now` is the signing time — pass a clock-skew-corrected time (Session
    // does this for you). `style` should already be resolved against the
    // provider profile. Fails with InvalidRequest for an expiry outside
    // 1…kMaxExpirySeconds or invalid credentials.
    [[nodiscard]] static Result<QUrl> presign(const Endpoint& endpoint, AddressingStyle style,
                                              const Credentials& credentials,
                                              const PresignRequest& request,
                                              const QDateTime& now);
};

} // namespace qbucket::s3
