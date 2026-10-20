#pragma once

#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/RequestObserver.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/providers/ProviderProfile.h>

#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

namespace qbucket::s3 {

// Handle of one request, for Session::cancel(). Invalid is never issued.
enum class RequestId : quint64 { Invalid = 0 };

template <typename T>
using Callback = std::function<void(Result<T>)>;

struct SessionOptions {
    Endpoint endpoint;
    std::shared_ptr<const ProviderProfile> profile;     // required
    std::shared_ptr<CredentialsProvider> credentials;   // required
    std::shared_ptr<RequestObserver> observer;          // optional

    int transferTimeoutMs = 120'000;    // no progress for this long → Transient error
    qint64 maxInMemoryBytes = Q_INT64_C(64) * 1024 * 1024;   // GetObject / responses
    int maxAttempts = 4;                // per request, for retryable failures
    int retryBaseDelayMs = 250;
    int captureResponseBodyBytes = 0;   // > 0: RequestRecord::responseBody (fixtures)
    QByteArray userAgent;               // empty: "qbucket-s3/<version>"
    // Server time minus local time to start with — e.g. the offset a
    // previous Session reported via clockSkewDetected. Corrected anyway.
    qint64 initialClockOffsetSecs = 0;
};

// The entry point of the library: one S3 endpoint + credentials, served by
// its own worker thread with its own QNetworkAccessManager. The calling
// thread never blocks on network I/O.
//
// Every request method:
//   - returns at once with a RequestId (Invalid only for a null context),
//   - invokes the callback exactly once, queued, on `context`'s thread —
//     never synchronously inside the call,
//   - drops the callback if `context` or the Session is destroyed first,
//   - delivers ErrorCategory::Cancelled after cancel(), shutdown() or for
//     requests made after shutdown().
//
// Thread-safety: the request methods, cancel() and cancelAll() may be
// called from any thread. Construction, destruction and shutdown() happen
// in the thread that owns the Session.
class QBUCKET_S3_EXPORT Session : public QObject {
    Q_OBJECT

public:
    explicit Session(SessionOptions options, QObject* parent = nullptr);
    // Cancels everything (no callbacks any more) and stops the worker,
    // waiting at most ~2 s. Prefer shutdown() + shutdownFinished in apps.
    ~Session() override;

    [[nodiscard]] const SessionOptions& options() const;

    RequestId listBuckets(QObject* context, Callback<BucketList> callback);
    RequestId listObjects(const ListObjectsParams& params, QObject* context,
                          Callback<ListPage> callback);
    RequestId headBucket(const QString& bucket, QObject* context, Callback<void> callback);
    RequestId getBucketLocation(const QString& bucket, QObject* context, Callback<QString> callback);
    RequestId headObject(const QString& bucket, const QString& key, const QString& versionId,
                         QObject* context, Callback<ObjectMeta> callback);
    RequestId getObject(const QString& bucket, const QString& key, const QString& versionId,
                        QObject* context, Callback<ObjectData> callback);

    // --- data-changing requests — applications route these through their
    //     own safety layer (confirmations, read-only accounts, journals) ---
    RequestId putObject(const PutObjectParams& params, QObject* context,
                        Callback<PutObjectResult> callback);
    RequestId deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                           QObject* context, Callback<DeleteObjectResult> callback);
    // One batch of at most ProviderProfile::maxDeleteBatch() objects.
    RequestId deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                            QObject* context, Callback<DeleteObjectsResult> callback);
    RequestId createBucket(const CreateBucketParams& params, QObject* context,
                           Callback<void> callback);
    RequestId deleteBucket(const QString& bucket, QObject* context, Callback<void> callback);

    // Presigned URL (no request is sent): current credentials, clock-skew
    // corrected time, the bucket's learned region, the profile's expiry cap.
    RequestId presign(const PresignRequest& request, QObject* context, Callback<QUrl> callback);

    void cancel(RequestId id);
    void cancelAll();

    // Orderly, non-blocking stop: cancels all requests, stops the worker and
    // emits shutdownFinished once it has.
    void shutdown();

Q_SIGNALS:
    // Local clock and server differ by offsetSecs; signing is corrected.
    void clockSkewDetected(qint64 offsetSecs);
    void shutdownFinished();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qbucket::s3
