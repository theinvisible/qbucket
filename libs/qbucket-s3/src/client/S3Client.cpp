#include "client/S3Client.h"

#include "core/HttpDate.h"
#include "xml/XmlParsers.h"
#include "xml/XmlWriters.h"

#include <QCryptographicHash>
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

Error invalid(const QString& message)
{
    return Error::make(ErrorCategory::InvalidRequest, message);
}

QByteArray contentMd5(const QByteArray& body)
{
    return QCryptographicHash::hash(body, QCryptographicHash::Md5).toBase64();
}

bool isPrintableAscii(const QString& text)
{
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7E)
            return false;
    }
    return true;
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

// Single-part, not KMS / SSE-C encrypted: then (and only then) the ETag is
// the MD5 of the content.
bool etagIsContentMd5(const QString& etag, const RawResponse& r)
{
    QString plain = etag;
    plain.remove(QLatin1Char('"'));
    if (plain.size() != 32 || plain.contains(QLatin1Char('-')))
        return false;
    for (const QChar c : plain) {
        if (!c.isDigit() && !(c >= QLatin1Char('a') && c <= QLatin1Char('f'))
            && !(c >= QLatin1Char('A') && c <= QLatin1Char('F')))
            return false;
    }
    if (r.header("x-amz-server-side-encryption") == "aws:kms"
        || r.header("x-amz-server-side-encryption") == "aws:kms:dsse"
        || !r.header("x-amz-server-side-encryption-customer-algorithm").isNull())
        return false;
    return true;
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

template <typename T, typename Parse>
quint64 S3Client::single(RequestSpec spec, Done<T> done, Parse parse)
{
    const auto op = newOp();
    send(op, std::move(spec),
         [this, op, done = std::move(done), parse = std::move(parse)](Result<RawResponse> r) {
             endOp(op);
             if (!r) {
                 done(r.error());
                 return;
             }
             done(parse(r.value()));
         });
    return op->id;
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

    return single(std::move(spec), std::move(done), [this, params, v1](const RawResponse& r) -> Result<ListPage> {
        const ProviderProfile& profile = *config().profile;
        const bool plus = profile.listingDecodesPlusAsSpace();
        Result<ListPage> page = v1 ? xml::parseListObjectsV1(r.body, plus)
                                   : xml::parseListObjectsV2(r.body, plus);
        if (!page)
            return page;
        if (page->isTruncated && !params.continuationToken.isEmpty()
            && page->nextContinuationToken == params.continuationToken) {
            return Error::make(ErrorCategory::Protocol,
                               QStringLiteral("listing returned the same continuation token again "
                                              "(listing loop)"));
        }
        if (page->bucket.isEmpty())
            page->bucket = params.bucket;
        profile.normalizeListing(page.value());
        return page;
    });
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
    return single(std::move(spec), std::move(done), [](const RawResponse&) { return Result<void>(); });
}

quint64 S3Client::getBucketLocation(const QString& bucket, Done<QString> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::GetBucketLocation;
    spec.bucket = bucket;
    spec.query.append({"location", {}, true});
    return single(std::move(spec), std::move(done), [this](const RawResponse& r) -> Result<QString> {
        const Result<QString> constraint = xml::parseLocationConstraint(r.body);
        if (!constraint)
            return constraint;
        return config().profile->regionFromLocationConstraint(constraint.value());
    });
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
    return single(std::move(spec), std::move(done), [key](const RawResponse& r) {
        return Result<ObjectMeta>(metaFromHeaders(key, r));
    });
}

// --- Objects -----------------------------------------------------------------------

quint64 S3Client::getObject(const QString& bucket, const QString& key, const QString& versionId,
                            Done<ObjectData> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec;
    spec.operation = Operation::GetObject;
    spec.bucket = bucket;
    spec.key = key;
    spec.versionId = versionId;
    if (!versionId.isEmpty())
        spec.query.append({"versionId", versionId.toUtf8(), false});
    // Never mayReturn200Error: the object itself may be an <Error> document.
    return single(std::move(spec), std::move(done), [key](const RawResponse& r) -> Result<ObjectData> {
        ObjectData out;
        out.meta = metaFromHeaders(key, r);
        out.data = r.body;
        if (out.meta.size >= 0 && out.meta.size != out.data.size()) {
            return Error::make(ErrorCategory::Transient,
                               QStringLiteral("received %1 bytes, expected %2")
                                   .arg(out.data.size())
                                   .arg(out.meta.size));
        }
        if (etagIsContentMd5(out.meta.etag, r)) {
            QString expected = out.meta.etag;
            expected.remove(QLatin1Char('"'));
            const QByteArray actual =
                QCryptographicHash::hash(out.data, QCryptographicHash::Md5).toHex();
            if (expected.toLatin1().toLower() != actual) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("integrity check failed: MD5 of the received "
                                                  "data does not match the ETag"));
            }
            out.integrity = Integrity::Md5Verified;
        }
        return out;
    });
}

quint64 S3Client::putObject(const PutObjectParams& params, Done<PutObjectResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));

    RequestSpec spec;
    spec.operation = Operation::PutObject;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.body = params.data;
    spec.contentType = params.contentType.isEmpty() ? QByteArray("application/octet-stream")
                                                    : params.contentType.toUtf8();
    // Retrying a PUT of the same bytes is safe (on versioned buckets it may
    // leave one extra version).
    spec.idempotent = true;
    spec.headers.append({"Content-MD5", contentMd5(params.data)});
    if (!params.storageClass.isEmpty())
        spec.headers.append({"x-amz-storage-class", params.storageClass.toLatin1()});
    for (auto it = params.metadata.cbegin(); it != params.metadata.cend(); ++it) {
        if (it.key().isEmpty() || !isPrintableAscii(it.key()) || !isPrintableAscii(it.value())) {
            return failLater(std::move(done),
                             invalid(QStringLiteral("metadata must be printable ASCII: ") + it.key()));
        }
        spec.headers.append({"x-amz-meta-" + it.key().toLower().toLatin1(), it.value().toLatin1()});
    }

    return single(std::move(spec), std::move(done), [](const RawResponse& r) {
        PutObjectResult out;
        out.etag = QString::fromLatin1(r.header("etag"));
        out.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        return Result<PutObjectResult>(out);
    });
}

quint64 S3Client::deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                               Done<DeleteObjectResult> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec;
    spec.operation = Operation::DeleteObject;
    spec.method = "DELETE";
    spec.bucket = bucket;
    spec.key = key;
    spec.versionId = versionId;
    if (!versionId.isEmpty())
        spec.query.append({"versionId", versionId.toUtf8(), false});
    return single(std::move(spec), std::move(done), [](const RawResponse& r) {
        DeleteObjectResult out;
        out.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        out.deleteMarker = r.header("x-amz-delete-marker") == "true";
        return Result<DeleteObjectResult>(out);
    });
}

quint64 S3Client::deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                                Done<DeleteObjectsResult> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    if (objects.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("nothing to delete")));
    if (objects.size() > config().profile->maxDeleteBatch()) {
        return failLater(std::move(done),
                         invalid(QStringLiteral("at most %1 objects per batch delete")
                                     .arg(config().profile->maxDeleteBatch())));
    }
    for (const ObjectIdentifier& o : objects) {
        if (o.key.isEmpty())
            return failLater(std::move(done), invalid(QStringLiteral("empty key in batch delete")));
    }
    Result<QByteArray> body = xml::buildDeleteRequest(objects, false);
    if (!body)
        return failLater(std::move(done), body.error());

    RequestSpec spec;
    spec.operation = Operation::DeleteObjects;
    spec.method = "POST";
    spec.bucket = bucket;
    spec.query.append({"delete", {}, true});
    spec.body = body.value();
    spec.contentType = "application/xml";
    spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    spec.mayReturn200Error = true;   // our own XML, never user data

    return single(std::move(spec), std::move(done), [objects](const RawResponse& r) -> Result<DeleteObjectsResult> {
        Result<DeleteObjectsResult> parsed = xml::parseDeleteResult(r.body);
        if (!parsed)
            return parsed;
        // Every requested key must be accounted for.
        QSet<QPair<QString, QString>> reported;
        for (const auto& d : parsed->deleted)
            reported.insert({d.key, d.versionId});
        for (const auto& f : parsed->failed)
            reported.insert({f.key, f.versionId});
        for (const ObjectIdentifier& o : objects) {
            const bool found = reported.contains({o.key, o.versionId})
                || (o.versionId.isEmpty()
                    && std::any_of(parsed->deleted.cbegin(), parsed->deleted.cend(),
                                   [&](const auto& d) { return d.key == o.key; }));
            if (!found) {
                DeleteObjectsResult::Failed f;
                f.key = o.key;
                f.versionId = o.versionId;
                f.code = QStringLiteral("NotReported");
                f.message = QStringLiteral("the server did not report this key");
                parsed->failed.append(f);
            }
        }
        return parsed;
    });
}

// --- Buckets -----------------------------------------------------------------------

quint64 S3Client::createBucket(const CreateBucketParams& params, Done<void> done)
{
    if (params.bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::CreateBucket;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    // A lost reply leaves it open whether the bucket now exists.
    spec.idempotent = false;
    const QString constraint = config().profile->locationConstraintFor(params.region);
    if (!constraint.isEmpty()) {
        spec.body = xml::buildCreateBucketConfiguration(constraint);
        spec.contentType = "application/xml";
    }
    if (params.objectLockEnabled)
        spec.headers.emplaceBack("x-amz-bucket-object-lock-enabled", "true");
    return single(std::move(spec), std::move(done), [](const RawResponse&) { return Result<void>(); });
}

quint64 S3Client::deleteBucket(const QString& bucket, Done<void> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::DeleteBucket;
    spec.method = "DELETE";
    spec.bucket = bucket;
    return single(std::move(spec), std::move(done), [](const RawResponse&) { return Result<void>(); });
}

} // namespace qbucket::s3
