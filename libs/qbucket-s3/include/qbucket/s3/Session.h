#pragma once

#include <qbucket/s3/Capabilities.h>
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

// Bytes sent or received by a request's current attempt, and the total (-1
// if unknown). Runs on the context's thread, at most ~10 times per second
// plus the final value, always before the request's callback. A retry
// starts again from 0.
using ProgressCallback = std::function<void(qint64 done, qint64 total)>;

struct SessionOptions {
    Endpoint endpoint;
    std::shared_ptr<const ProviderProfile> profile;     // required
    std::shared_ptr<CredentialsProvider> credentials;   // required
    std::shared_ptr<RequestObserver> observer;          // optional

    int transferTimeoutMs = 120'000;    // no progress for this long → Transient error
    // The same for requests the server works on before it answers —
    // CompleteMultipartUpload (VersityGW took minutes to assemble 50 GiB
    // without sending a byte), CopyObject, UploadPartCopy.
    int serverWorkTimeoutMs = 900'000;
    qint64 maxInMemoryBytes = Q_INT64_C(64) * 1024 * 1024;   // GetObject / responses
    int maxAttempts = 4;                // per request, for retryable failures
    int retryBaseDelayMs = 250;
    int captureResponseBodyBytes = 0;   // > 0: RequestRecord::responseBody (fixtures)
    QByteArray userAgent;               // empty: "qbucket-s3/<version>"
    // Server time minus local time to start with — e.g. the offset a
    // previous Session reported via clockSkewDetected. Corrected anyway.
    qint64 initialClockOffsetSecs = 0;
    // Extra worker threads, each with its own QNetworkAccessManager, for
    // transfer requests (file bodies, downloads to files) — so browsing
    // never queues behind transfers and the per-QNAM connection limit
    // applies per lane. Started on demand. 0: transfers share the browse
    // thread.
    int transferLanes = 0;
};

// The entry point of the library: one S3 endpoint + credentials, served by
// its own worker thread with its own QNetworkAccessManager (plus optional
// transfer lanes, see SessionOptions::transferLanes). The calling thread
// never blocks on network I/O.
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
    // With a part number (layout of a multipart object) or If-Match.
    RequestId headObject(const HeadObjectParams& params, QObject* context, Callback<ObjectMeta> callback);
    RequestId getObject(const QString& bucket, const QString& key, const QString& versionId,
                        QObject* context, Callback<ObjectData> callback);
    // Streams the object (or a range / part of it) into a local file on a
    // transfer lane; see GetObjectToFileParams and DownloadResult.
    RequestId getObjectToFile(const GetObjectToFileParams& params, QObject* context,
                              Callback<DownloadResult> callback, ProgressCallback progress = {});

    // Streamed bodies (FileBody) need FileBody::sha256Hex when true: plain
    // HTTP, or a provider that refuses UNSIGNED-PAYLOAD. Thread-safe.
    [[nodiscard]] bool requiresPayloadHash() const;

    // --- data-changing requests — applications route these through their
    //     own safety layer (confirmations, read-only accounts, journals) ---
    // With PutObjectParams::file the body is streamed on a transfer lane.
    RequestId putObject(const PutObjectParams& params, QObject* context,
                        Callback<PutObjectResult> callback, ProgressCallback progress = {});
    RequestId deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                           QObject* context, Callback<DeleteObjectResult> callback);
    // One batch of at most ProviderProfile::maxDeleteBatch() objects.
    RequestId deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                            QObject* context, Callback<DeleteObjectsResult> callback);
    // Server-side copy (≤ 5 GiB). A lost answer is reported with
    // Error::outcomeUnknown and not retried: look the target up.
    RequestId copyObject(const CopyObjectParams& params, QObject* context, Callback<CopyObjectResult> callback);
    // Multipart uploads (Feature::MultipartUpload). Parts are streamed on a
    // transfer lane. Complete is never retried after a lost answer
    // (outcomeUnknown): look the object up. Abort answers NotFound
    // (NoSuchUpload) once the upload is gone.
    RequestId createMultipartUpload(const CreateMultipartUploadParams& params, QObject* context,
                                    Callback<CreateMultipartUploadResult> callback);
    RequestId uploadPart(const UploadPartParams& params, QObject* context, Callback<UploadPartResult> callback,
                         ProgressCallback progress = {});
    RequestId completeMultipartUpload(const CompleteMultipartUploadParams& params, QObject* context,
                                      Callback<CompleteMultipartUploadResult> callback);
    RequestId abortMultipartUpload(const QString& bucket, const QString& key, const QString& uploadId,
                                   QObject* context, Callback<void> callback);
    // One page of an upload's parts / of a bucket's unfinished uploads
    // (Feature::MultipartListing); a page that makes no progress is a
    // Protocol error.
    RequestId listParts(const ListPartsParams& params, QObject* context, Callback<ListPartsPage> callback);
    RequestId listMultipartUploads(const ListMultipartUploadsParams& params, QObject* context,
                                   Callback<ListMultipartUploadsPage> callback);
    // Server-side copy of a source range into one part (on a transfer lane).
    RequestId uploadPartCopy(const UploadPartCopyParams& params, QObject* context,
                             Callback<UploadPartCopyResult> callback);

    RequestId createBucket(const CreateBucketParams& params, QObject* context,
                           Callback<void> callback);
    RequestId deleteBucket(const QString& bucket, QObject* context, Callback<void> callback);

    // Presigned URL (no request is sent): current credentials, clock-skew
    // corrected time, the bucket's learned region, the profile's expiry cap.
    RequestId presign(const PresignRequest& request, QObject* context, Callback<QUrl> callback);

    // --- capability safeguard (PLAN.md §3.3) — read-only probes ---
    // Server scope: access + fingerprint, with the profile's static knowledge.
    RequestId probeServer(QObject* context, Callback<CapabilitySet> callback);
    // Bucket scope: every GET subresource, object-level probes on one key;
    // bucket state (versioning, object lock). About one second.
    RequestId probeBucket(const QString& bucket, QObject* context,
                          Callback<CapabilitySet> callback);
    // Same, with an existing object for the object-level probes (otherwise
    // the first key the bucket lists).
    RequestId probeBucket(const QString& bucket, const QString& objectKey, QObject* context,
                          Callback<CapabilitySet> callback);
    // Known capabilities (e.g. merged and persisted by the app). Requests
    // needing an Unsupported feature then fail with ErrorCategory::Unsupported
    // without being sent. Thread-safe.
    void setCapabilities(const CapabilitySet& set);

    void cancel(RequestId id);
    void cancelAll();

    // Orderly, non-blocking stop: cancels all requests, stops the worker and
    // emits shutdownFinished once it has.
    void shutdown();

Q_SIGNALS:
    // Local clock and server differ by offsetSecs; signing is corrected.
    void clockSkewDetected(qint64 offsetSecs);
    // A real request showed a feature to be unsupported (e.g. 501); it is
    // refused locally from now on. featureId as in featureId(Feature).
    void capabilityLearned(const QString& bucket, const QString& featureId);
    void shutdownFinished();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qbucket::s3
