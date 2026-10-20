#include "client/S3Client.h"

#include "core/HttpDate.h"
#include "xml/XmlParsers.h"

#include <QSet>
#include <QTimer>

namespace qbucket::s3 {

struct S3Client::Op {
    quint64 id = 0;
    quint64 currentRequest = 0;
    bool cancelled = false;
};

namespace {

Error cancelledError()
{
    return Error::make(ErrorCategory::Cancelled, QStringLiteral("operation cancelled"));
}

ObjectMeta metaFromHeaders(const QString& key, const RawResponse& r)
{
    ObjectMeta m;
    m.key = key;
    const QByteArray length = r.header("content-length");
    m.size = length.isNull() ? -1 : length.toLongLong();
    m.lastModified = parseHttpDate(r.header("last-modified"));
    m.etag = QString::fromLatin1(r.header("etag"));
    m.contentType = QString::fromUtf8(r.header("content-type"));
    m.storageClass = QString::fromLatin1(r.header("x-amz-storage-class"));
    m.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
    static constexpr char kMetaPrefix[] = "x-amz-meta-";
    for (const auto& [name, value] : r.headers) {
        const QByteArray lower = name.toLower();
        if (lower.startsWith(kMetaPrefix)) {
            m.userMetadata.insert(QString::fromUtf8(lower.mid(int(sizeof(kMetaPrefix)) - 1)),
                                  QString::fromUtf8(value));
        }
    }
    return m;
}

Error invalid(const QString& message)
{
    return Error::make(ErrorCategory::InvalidRequest, message);
}

} // namespace

S3Client::S3Client(ClientConfig config, QObject* parent)
    : QObject(parent)
    , m_pipeline(new RequestPipeline(std::move(config), this))
{
}

S3Client::~S3Client() = default;

std::shared_ptr<S3Client::Op> S3Client::newOp()
{
    auto op = std::make_shared<Op>();
    op->id = m_nextOpId++;
    m_ops.emplace(op->id, op);
    return op;
}

void S3Client::endOp(const std::shared_ptr<Op>& op)
{
    m_ops.erase(op->id);
}

template <typename T>
quint64 S3Client::failLater(Done<T> done, Error error)
{
    QTimer::singleShot(0, this, [done = std::move(done), error = std::move(error)] { done(error); });
    return m_nextOpId++;   // a valid id that abort() simply won't find
}

void S3Client::send(const std::shared_ptr<Op>& op, RequestSpec spec,
                    std::function<void(Result<RawResponse>)> next)
{
    if (op->cancelled) {
        QTimer::singleShot(0, this, [next = std::move(next)] { next(cancelledError()); });
        return;
    }
    op->currentRequest = m_pipeline->execute(
        std::move(spec), [op, next = std::move(next)](Result<RawResponse> r) {
            op->currentRequest = 0;
            if (op->cancelled && r)
                r = cancelledError();
            next(std::move(r));
        });
}

void S3Client::abort(quint64 operationId)
{
    const auto it = m_ops.find(operationId);
    if (it == m_ops.end())
        return;
    it->second->cancelled = true;
    if (it->second->currentRequest)
        m_pipeline->abort(it->second->currentRequest);
}

void S3Client::abortAll()
{
    std::vector<quint64> ids;
    for (const auto& [id, op] : m_ops)
        ids.push_back(id);
    for (const quint64 id : ids)
        abort(id);
}

// --- ListBuckets ---------------------------------------------------------------

quint64 S3Client::listBuckets(Done<BucketList> done)
{
    const auto op = newOp();
    listBucketsPage(op, QString(), BucketList(), QSet<QString>(), std::move(done));
    return op->id;
}

void S3Client::listBucketsPage(const std::shared_ptr<Op>& op, const QString& token, BucketList acc,
                               QSet<QString> seenTokens, Done<BucketList> done)
{
    RequestSpec spec;
    spec.operation = Operation::ListBuckets;
    if (!token.isEmpty())
        spec.query.append({"continuation-token", token.toUtf8(), false});

    send(op, std::move(spec),
         [this, op, acc = std::move(acc), seenTokens = std::move(seenTokens),
          done = std::move(done)](Result<RawResponse> r) mutable {
             if (!r) {
                 endOp(op);
                 done(r.error());
                 return;
             }
             QString next;
             Result<BucketList> page = xml::parseListBuckets(r->body, &next);
             if (!page) {
                 endOp(op);
                 done(page.error());
                 return;
             }
             if (acc.ownerId.isEmpty()) {
                 acc.ownerId = page->ownerId;
                 acc.ownerDisplayName = page->ownerDisplayName;
             }
             acc.buckets += page->buckets;
             if (next.isEmpty()) {
                 endOp(op);
                 done(std::move(acc));
                 return;
             }
             if (seenTokens.contains(next)) {
                 endOp(op);
                 done(Error::make(ErrorCategory::Protocol,
                                  QStringLiteral("ListBuckets returned a continuation token twice "
                                                 "(listing loop)")));
                 return;
             }
             seenTokens.insert(next);
             listBucketsPage(op, next, std::move(acc), std::move(seenTokens), std::move(done));
         });
}

// --- ListObjects -----------------------------------------------------------------

quint64 S3Client::listObjects(const ListObjectsParams& params, Done<ListPage> done)
{
    if (params.bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));

    const ProviderProfile& profile = *config().profile;
    const bool v1 = profile.listApi() == ListApi::V1;
    const int maxKeys = std::clamp(params.maxKeys, 1, profile.maxKeysPerList());

    RequestSpec spec;
    spec.operation = v1 ? Operation::ListObjectsV1 : Operation::ListObjectsV2;
    spec.bucket = params.bucket;
    // Always url-encoded: keys may hold characters XML can't carry.
    spec.query.append({"encoding-type", "url", false});
    spec.query.append({"max-keys", QByteArray::number(maxKeys), false});
    spec.query.append({"prefix", params.prefix.toUtf8(), false});
    if (!params.delimiter.isEmpty())
        spec.query.append({"delimiter", params.delimiter.toUtf8(), false});
    if (v1) {
        const QString marker = params.continuationToken.isEmpty() ? params.startAfter
                                                                  : params.continuationToken;
        if (!marker.isEmpty())
            spec.query.append({"marker", marker.toUtf8(), false});
    } else {
        spec.query.append({"list-type", "2", false});
        if (!params.continuationToken.isEmpty())
            spec.query.append({"continuation-token", params.continuationToken.toUtf8(), false});
        if (!params.startAfter.isEmpty())
            spec.query.append({"start-after", params.startAfter.toUtf8(), false});
    }

    const auto op = newOp();
    send(op, std::move(spec), [this, op, params, v1, done = std::move(done)](Result<RawResponse> r) {
        endOp(op);
        if (!r) {
            done(r.error());
            return;
        }
        const ProviderProfile& profile = *config().profile;
        const bool plus = profile.listingDecodesPlusAsSpace();
        Result<ListPage> page = v1 ? xml::parseListObjectsV1(r->body, plus)
                                   : xml::parseListObjectsV2(r->body, plus);
        if (!page) {
            done(page.error());
            return;
        }
        if (page->isTruncated && !params.continuationToken.isEmpty()
            && page->nextContinuationToken == params.continuationToken) {
            done(Error::make(ErrorCategory::Protocol,
                             QStringLiteral("listing returned the same continuation token again "
                                            "(listing loop)")));
            return;
        }
        if (page->bucket.isEmpty())
            page->bucket = params.bucket;
        profile.normalizeListing(page.value());
        done(std::move(page));
    });
    return op->id;
}

// --- Bucket / object metadata -----------------------------------------------------

quint64 S3Client::headBucket(const QString& bucket, Done<void> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::HeadBucket;
    spec.method = "HEAD";
    spec.bucket = bucket;
    const auto op = newOp();
    send(op, std::move(spec), [this, op, done = std::move(done)](Result<RawResponse> r) {
        endOp(op);
        if (!r) {
            done(r.error());
            return;
        }
        done({});
    });
    return op->id;
}

quint64 S3Client::getBucketLocation(const QString& bucket, Done<QString> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::GetBucketLocation;
    spec.bucket = bucket;
    spec.query.append({"location", {}, true});
    const auto op = newOp();
    send(op, std::move(spec), [this, op, done = std::move(done)](Result<RawResponse> r) {
        endOp(op);
        if (!r) {
            done(r.error());
            return;
        }
        const Result<QString> constraint = xml::parseLocationConstraint(r->body);
        if (!constraint) {
            done(constraint.error());
            return;
        }
        done(config().profile->regionFromLocationConstraint(constraint.value()));
    });
    return op->id;
}

quint64 S3Client::headObject(const QString& bucket, const QString& key, const QString& versionId,
                             Done<ObjectMeta> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec;
    spec.operation = Operation::HeadObject;
    spec.method = "HEAD";
    spec.bucket = bucket;
    spec.key = key;
    spec.versionId = versionId;
    if (!versionId.isEmpty())
        spec.query.append({"versionId", versionId.toUtf8(), false});
    const auto op = newOp();
    send(op, std::move(spec), [this, op, key, done = std::move(done)](Result<RawResponse> r) {
        endOp(op);
        if (!r) {
            done(r.error());
            return;
        }
        done(metaFromHeaders(key, r.value()));
    });
    return op->id;
}

} // namespace qbucket::s3
