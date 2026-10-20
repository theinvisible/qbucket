#pragma once

#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace qbucket::s3 {

// One finished HTTP exchange (one attempt), already redacted: no
// Authorization header, no signatures, no tokens. Value type; reentrant.
struct RequestRecord {
    QString operation;        // "ListObjectsV2", "PutObject", …
    QByteArray method;        // "GET", "PUT", …
    QString bucket;
    QString key;
    QString versionId;
    QString url;              // redacted
    bool mutating = false;    // changes data or configuration on the server
    int attempt = 1;          // 1 for the first try, 2+ for retries

    QDateTime startedAt;      // UTC
    qint64 durationMs = 0;
    qint64 bytesSent = 0;
    qint64 bytesReceived = 0;

    int httpStatus = 0;       // 0: no response (network error, cancelled)
    QString requestId;        // x-amz-request-id
    bool ok = false;
    Error error;              // valid when !ok

    // Only filled when body capture is enabled on the Session (fixture
    // capture, diagnostics); capped in size. Never contains request bodies.
    QByteArray responseBody;
};

// Audit hook: sees every request the library sends (QBucket's journal
// implements it). Implementations must be thread-safe — onRequestFinished is
// called on the library's worker threads — and must return quickly.
class QBUCKET_S3_EXPORT RequestObserver {
public:
    virtual ~RequestObserver();

    virtual void onRequestFinished(const RequestRecord& record) = 0;

protected:
    RequestObserver() = default;
    RequestObserver(const RequestObserver&) = default;
    RequestObserver& operator=(const RequestObserver&) = default;
};

} // namespace qbucket::s3
