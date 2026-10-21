#pragma once

#include "client/RequestPipeline.h"

#include <qbucket/s3/Capabilities.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>

#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace qbucket::s3 {

// The S3 operations, on top of RequestPipeline: builds requests, parses
// responses, walks multi-request operations (paginated ListBuckets).
//
// Private API — consumers use Session, which runs an S3Client on a worker
// thread. Thread affinity: lives on one thread; all calls and all callbacks
// happen there. Callbacks are never invoked synchronously.
class QBUCKET_S3_EXPORT S3Client : public QObject {
    Q_OBJECT

public:
    template <typename T>
    using Done = std::function<void(Result<T>)>;
    // Bytes of the current attempt so far, total (-1 unknown); our thread.
    using Progress = std::function<void(qint64 done, qint64 total)>;

    explicit S3Client(ClientConfig config, QObject* parent = nullptr);
    ~S3Client() override;

    [[nodiscard]] const ClientConfig& config() const { return m_pipeline->config(); }

    // Each returns an operation id for abort().
    quint64 listBuckets(Done<BucketList> done);
    quint64 listObjects(const ListObjectsParams& params, Done<ListPage> done);
    quint64 listObjectVersions(const ListObjectVersionsParams& params, Done<ListObjectVersionsPage> done);
    quint64 getBucketVersioning(const QString& bucket, Done<BucketVersioning> done);
    quint64 putBucketVersioning(const PutBucketVersioningParams& params, Done<SubresourceWriteResult> done);
    quint64 headBucket(const QString& bucket, Done<void> done);
    // Region of the bucket, already mapped through the provider profile.
    quint64 getBucketLocation(const QString& bucket, Done<QString> done);
    quint64 headObject(const QString& bucket, const QString& key, const QString& versionId,
                       Done<ObjectMeta> done);
    quint64 headObject(const HeadObjectParams& params, Done<ObjectMeta> done);

    // In memory, up to ClientConfig::maxResponseBytes; MD5-verified against
    // the ETag where that is meaningful (see Integrity). Streaming to files
    // is the transfer engine's job.
    quint64 getObject(const QString& bucket, const QString& key, const QString& versionId,
                      Done<ObjectData> done);
    // Streamed into a file (range, part or whole object); see
    // GetObjectToFileParams. A server that ignores the Range header is an
    // error, never silently a whole object at the wrong offset.
    quint64 getObjectToFile(const GetObjectToFileParams& params, Done<DownloadResult> done,
                            Progress progress = {});
    // Single PUT with Content-MD5, from memory or streamed from a file.
    quint64 putObject(const PutObjectParams& params, Done<PutObjectResult> done, Progress progress = {});
    quint64 deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                         Done<DeleteObjectResult> done);
    // CopyObject (≤ 5 GiB), directive COPY or REPLACE; 200-with-error aware; never
    // retried after a lost answer (outcomeUnknown).
    quint64 copyObject(const CopyObjectParams& params, Done<CopyObjectResult> done);
    // Tags and ACL of an object. The writes check that the answer isn't one
    // of an object upload (an ETag): a proxy that dropped the subresource
    // would have stored the request as the object.
    quint64 getObjectTagging(const QString& bucket, const QString& key, const QString& versionId,
                             Done<ObjectTagging> done);
    quint64 putObjectTagging(const PutObjectTaggingParams& params, Done<SubresourceWriteResult> done);
    quint64 getObjectAcl(const QString& bucket, const QString& key, const QString& versionId,
                         Done<AccessControlPolicy> done);
    quint64 putObjectAcl(const PutObjectAclParams& params, Done<SubresourceWriteResult> done);
    // One batch (at most ProviderProfile::maxDeleteBatch()), with Content-MD5.
    // Every requested key ends up in deleted or failed — keys the server did
    // not report are listed as failed ("NotReported").
    quint64 deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                          Done<DeleteObjectsResult> done);
    // --- multipart uploads (PLAN.md §12.5) ---
    quint64 createMultipartUpload(const CreateMultipartUploadParams& params,
                                  Done<CreateMultipartUploadResult> done);
    // Streamed from a file range with Content-MD5; retried like any PUT.
    quint64 uploadPart(const UploadPartParams& params, Done<UploadPartResult> done, Progress progress = {});
    // 200-with-error aware; never retried after a lost answer
    // (outcomeUnknown) — the caller looks the object up instead.
    quint64 completeMultipartUpload(const CompleteMultipartUploadParams& params,
                                    Done<CompleteMultipartUploadResult> done);
    // NotFound (NoSuchUpload) when the upload is gone — also after a retry
    // of an abort whose first answer was lost.
    quint64 abortMultipartUpload(const QString& bucket, const QString& key, const QString& uploadId,
                                 Done<void> done);
    quint64 listParts(const ListPartsParams& params, Done<ListPartsPage> done);
    quint64 listMultipartUploads(const ListMultipartUploadsParams& params, Done<ListMultipartUploadsPage> done);
    quint64 uploadPartCopy(const UploadPartCopyParams& params, Done<UploadPartCopyResult> done);

    // Bucket lifecycle — exposed for tooling and tests; applications gate
    // these behind their own confirmation/policy layer.
    quint64 createBucket(const CreateBucketParams& params, Done<void> done);
    quint64 deleteBucket(const QString& bucket, Done<void> done);

    // Presigned URL with the current credentials, the clock-skew-corrected
    // time and the bucket's learned region; expiry capped by the profile.
    quint64 presign(const PresignRequest& request, Done<QUrl> done);

    // --- capability safeguard (PLAN.md §3.3) ---
    // Read-only probes. Server scope: ListBuckets (fingerprint, access).
    // Bucket scope: every GET subresource + object-level probes on one key.
    quint64 probeServer(Done<CapabilitySet> done);
    // objectKey: an existing object for the object-level probes; empty: the
    // first key the bucket lists.
    quint64 probeBucket(const QString& bucket, const QString& objectKey, Done<CapabilitySet> done);
    // Known capabilities (server or bucket scope). A request needing a
    // feature that is Unsupported fails at once with Error Unsupported,
    // without being sent.
    void setCapabilities(const CapabilitySet& set);

    void abort(quint64 operationId);
    void abortAll();

Q_SIGNALS:
    void clockSkewDetected(qint64 offsetSecs);
    // A real request revealed a feature as Unsupported (e.g. 501).
    void capabilityLearned(const QString& bucket, const QString& featureId);

private:
    struct Op {
        quint64 id = 0;
        std::set<quint64> requests;   // in flight (probes run several at once)
        bool cancelled = false;
    };

    // Runs one request as (part of) operation `op`.
    void send(const std::shared_ptr<Op>& op, RequestSpec spec,
              std::function<void(Result<RawResponse>)> next);
    std::shared_ptr<Op> newOp();
    void endOp(const std::shared_ptr<Op>& op);
    template <typename T>
    quint64 failLater(Done<T> done, Error error)
    {
        QTimer::singleShot(0, this, [done = std::move(done), error = std::move(error)] { done(error); });
        return m_nextOpId++;   // a valid id that abort() simply won't find
    }
    // One request → parse(RawResponse) → done.
    template <typename T, typename Parse>
    quint64 single(RequestSpec&& spec, Done<T> done, Parse parse);

    std::optional<Error> shortCircuit(const RequestSpec& spec) const;
    // Bucket over server over profile.
    CapabilityEntry effectiveEntry(Feature feature, const QString& bucket) const;
    void learnFrom(const RequestSpec& spec, const Error& error);
    struct ProbeRun;
    void runProbes(const std::shared_ptr<ProbeRun>& run);

    void listBucketsPage(const std::shared_ptr<Op>& op, const QString& token, BucketList acc,
                         QSet<QString> seenTokens, Done<BucketList> done);

    RequestPipeline* m_pipeline;
    CapabilitySet m_serverCapabilities;
    QHash<QString, CapabilitySet> m_bucketCapabilities;
    std::map<quint64, std::shared_ptr<Op>> m_ops;
    quint64 m_nextOpId = 1;
};

} // namespace qbucket::s3
