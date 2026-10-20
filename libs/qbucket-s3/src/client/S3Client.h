#pragma once

#include "client/RequestPipeline.h"

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>

#include <QObject>

#include <functional>
#include <map>
#include <memory>

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

    explicit S3Client(ClientConfig config, QObject* parent = nullptr);
    ~S3Client() override;

    [[nodiscard]] const ClientConfig& config() const { return m_pipeline->config(); }

    // Each returns an operation id for abort().
    quint64 listBuckets(Done<BucketList> done);
    quint64 listObjects(const ListObjectsParams& params, Done<ListPage> done);
    quint64 headBucket(const QString& bucket, Done<void> done);
    // Region of the bucket, already mapped through the provider profile.
    quint64 getBucketLocation(const QString& bucket, Done<QString> done);
    quint64 headObject(const QString& bucket, const QString& key, const QString& versionId,
                       Done<ObjectMeta> done);

    // In memory, up to ClientConfig::maxResponseBytes; MD5-verified against
    // the ETag where that is meaningful (see Integrity). Streaming to files
    // is the transfer engine's job.
    quint64 getObject(const QString& bucket, const QString& key, const QString& versionId,
                      Done<ObjectData> done);
    // Single PUT with Content-MD5.
    quint64 putObject(const PutObjectParams& params, Done<PutObjectResult> done);
    quint64 deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                         Done<DeleteObjectResult> done);
    // One batch (at most ProviderProfile::maxDeleteBatch()), with Content-MD5.
    // Every requested key ends up in deleted or failed — keys the server did
    // not report are listed as failed ("NotReported").
    quint64 deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                          Done<DeleteObjectsResult> done);
    // Bucket lifecycle — exposed for tooling and tests; applications gate
    // these behind their own confirmation/policy layer.
    quint64 createBucket(const CreateBucketParams& params, Done<void> done);
    quint64 deleteBucket(const QString& bucket, Done<void> done);

    void abort(quint64 operationId);
    void abortAll();

private:
    struct Op;

    // Runs one request as (part of) operation `op`.
    void send(const std::shared_ptr<Op>& op, RequestSpec spec,
              std::function<void(Result<RawResponse>)> next);
    std::shared_ptr<Op> newOp();
    void endOp(const std::shared_ptr<Op>& op);
    template <typename T>
    quint64 failLater(Done<T> done, Error error);
    // One request → parse(RawResponse) → done.
    template <typename T, typename Parse>
    quint64 single(RequestSpec spec, Done<T> done, Parse parse);

    void listBucketsPage(const std::shared_ptr<Op>& op, const QString& token, BucketList acc,
                         QSet<QString> seenTokens, Done<BucketList> done);

    RequestPipeline* m_pipeline;
    std::map<quint64, std::shared_ptr<Op>> m_ops;
    quint64 m_nextOpId = 1;
};

} // namespace qbucket::s3
