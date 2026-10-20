#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/transfer/Export.h>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QThreadPool;

namespace qbucket::s3::transfer {

// Hashes of one file range. Raw digests (16 / 32 bytes).
struct RangeHash {
    qint64 bytes = 0;
    QByteArray md5;
    QByteArray sha256;   // only when asked for
};

// Set to stop a running hash; the callback then gets Cancelled.
using CancelFlag = std::shared_ptr<std::atomic<bool>>;

// Hashes a range of a local file on a thread pool (PLAN.md §12.5: the hash
// is a pre-pass on a worker, the part is then streamed). The callback runs
// once, queued, on `context`'s thread, and is dropped when the context is
// gone. A file shorter than the range is a LocalIo error. Call it on the
// context's thread; pool: QThreadPool::globalInstance() when null.
QBUCKET_S3_TRANSFER_EXPORT CancelFlag hashRange(const QString& path, qint64 offset, qint64 length,
                                                bool withSha256, QObject* context,
                                                std::function<void(Result<RangeHash>)> callback,
                                                QThreadPool* pool = nullptr);

// The synchronous core of hashRange, for worker threads.
QBUCKET_S3_TRANSFER_EXPORT Result<RangeHash> hashRangeNow(const QString& path, qint64 offset, qint64 length,
                                                          bool withSha256, const std::atomic<bool>* cancel = nullptr);

// --- ETags -----------------------------------------------------------------------

// "\"<hex>\"" for a raw MD5.
QBUCKET_S3_TRANSFER_EXPORT QString md5Etag(const QByteArray& rawMd5);
// S3's multipart ETag: MD5 of the concatenated part MD5s, "-<count>", quoted.
QBUCKET_S3_TRANSFER_EXPORT QString compositeEtag(const QList<QByteArray>& partMd5s);
// Parts of a multipart ETag ("…-12" → 12); 0 for a plain one, -1 if malformed.
QBUCKET_S3_TRANSFER_EXPORT int etagPartCount(const QString& etag);
// Compares ETags without quotes and case.
QBUCKET_S3_TRANSFER_EXPORT bool sameEtag(const QString& a, const QString& b);

} // namespace qbucket::s3::transfer
