#include "client/S3Client.h"

#include "http/UriEncoding.h"

#include "core/HttpDate.h"
#include "xml/XmlParsers.h"
#include "xml/XmlWriters.h"

#include <QCryptographicHash>
#include <QSet>
#include <QTimer>

#include <set>

namespace qbucket::s3 {

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
        m.responseHeaders.append({QString::fromLatin1(name), QString::fromUtf8(value)});
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
    connect(m_pipeline, &RequestPipeline::clockSkewDetected, this, &S3Client::clockSkewDetected);
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

template <typename T, typename Parse>
quint64 S3Client::single(RequestSpec&& spec, Done<T> done, Parse parse)
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
    if (std::optional<Error> blocked = shortCircuit(spec)) {
        QTimer::singleShot(0, this, [next = std::move(next), e = std::move(*blocked)] { next(e); });
        return;
    }
    const RequestSpec sent = spec;   // operation + bucket, for learning
    auto holder = std::make_shared<quint64>(0);
    *holder = m_pipeline->execute(
        std::move(spec), [this, op, holder, sent, next = std::move(next)](Result<RawResponse> r) {
            op->requests.erase(*holder);
            if (!r)
                learnFrom(sent, r.error());
            if (op->cancelled && r)
                r = cancelledError();
            next(std::move(r));
        });
    op->requests.insert(*holder);
}

std::optional<Error> S3Client::shortCircuit(const RequestSpec& spec) const
{
    const QList<Feature> needed = featuresFor(spec.operation);
    if (needed.isEmpty())
        return std::nullopt;
    const CapabilitySet profile = config().profile->capabilities();
    const auto bucketIt = m_bucketCapabilities.constFind(spec.bucket);
    for (const Feature f : needed) {
        CapabilityEntry e = bucketIt != m_bucketCapabilities.constEnd()
            ? capabilities::effective(f, *bucketIt, m_serverCapabilities)
            : m_serverCapabilities.entry(f);
        if (e.state == CapabilityState::Unknown)
            e = profile.entry(f);
        if (e.state == CapabilityState::Unsupported) {
            Error err = Error::make(ErrorCategory::Unsupported,
                                    QStringLiteral("the server does not support %1").arg(featureId(f)));
            err.resource = spec.bucket;
            err.code = QStringLiteral("CapabilityUnsupported");
            return err;
        }
    }
    return std::nullopt;
}

void S3Client::learnFrom(const RequestSpec& spec, const Error& error)
{
    if (error.category != ErrorCategory::Unsupported || spec.bucket.isEmpty())
        return;
    for (const Feature f : featuresFor(spec.operation)) {
        CapabilitySet& set = m_bucketCapabilities[spec.bucket];
        set.scope = CapabilityScope::Bucket;
        set.bucket = spec.bucket;
        set.set(f, {CapabilityState::Unsupported, CapabilitySource::Learned,
                    QDateTime::currentDateTimeUtc(), error.httpStatus, error.code, false, 0});
        Q_EMIT capabilityLearned(spec.bucket, featureId(f));
    }
}

void S3Client::setCapabilities(const CapabilitySet& set)
{
    if (set.scope == CapabilityScope::Server)
        m_serverCapabilities = set;
    else
        m_bucketCapabilities.insert(set.bucket, set);
}

void S3Client::abort(quint64 operationId)
{
    const auto it = m_ops.find(operationId);
    if (it == m_ops.end())
        return;
    it->second->cancelled = true;
    const std::set<quint64> requests = it->second->requests;
    for (const quint64 r : requests)
        m_pipeline->abort(r);
}

void S3Client::abortAll()
{
    std::vector<quint64> ids;
    ids.reserve(m_ops.size());
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
        Result<QString> constraint = xml::parseLocationConstraint(r.body);
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

quint64 S3Client::getObjectToFile(const GetObjectToFileParams& params, Done<DownloadResult> done,
                                  Progress progress)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    if (params.filePath.isEmpty() || params.fileOffset < 0)
        return failLater(std::move(done), invalid(QStringLiteral("no target file")));
    const bool ranged = params.rangeLength >= 0;
    if (ranged && (params.rangeStart < 0 || params.rangeLength == 0))
        return failLater(std::move(done), invalid(QStringLiteral("invalid byte range")));
    if (ranged && params.partNumber > 0)
        return failLater(std::move(done), invalid(QStringLiteral("a range and a part number exclude each other")));

    RequestSpec spec;
    spec.operation = Operation::GetObject;
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.versionId = params.versionId;
    if (!params.versionId.isEmpty())
        spec.query.append({"versionId", params.versionId.toUtf8(), false});
    if (params.partNumber > 0)
        spec.query.append({"partNumber", QByteArray::number(params.partNumber), false});
    if (ranged) {
        spec.headers.append({"Range", "bytes=" + QByteArray::number(params.rangeStart) + '-'
                                          + QByteArray::number(params.rangeStart + params.rangeLength - 1)});
    }
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"If-Match", params.ifMatch.toLatin1()});
    spec.sinkPath = params.filePath;
    spec.sinkOffset = params.fileOffset;
    spec.progress = std::move(progress);

    return single(std::move(spec), std::move(done), [params, ranged](const RawResponse& r) -> Result<DownloadResult> {
        DownloadResult out;
        out.meta = metaFromHeaders(params.key, r);
        out.bytesWritten = r.sinkBytes;
        out.md5 = r.sinkMd5;
        out.throttled = r.throttled;
        bool ok = false;
        const int parts = r.header("x-amz-mp-parts-count").trimmed().toInt(&ok);
        out.partsCount = ok ? parts : 0;

        // "bytes 0-99/1234": the object's size; "*" when unknown.
        const QByteArray contentRange = r.header("content-range").trimmed();
        if (const qsizetype slash = contentRange.lastIndexOf('/'); slash >= 0) {
            const qint64 total = contentRange.mid(slash + 1).toLongLong(&ok);
            if (ok)
                out.objectSize = total;
        }
        if (ranged) {
            if (r.status != 206) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("the server ignored the Range header (status %1)").arg(r.status));
            }
            const QByteArray expected = "bytes " + QByteArray::number(params.rangeStart) + '-';
            if (!contentRange.startsWith(expected)) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("the server answered another range: ")
                                       + QString::fromLatin1(contentRange));
            }
        } else if (params.partNumber <= 0 && out.objectSize < 0) {
            out.objectSize = out.meta.size;
        }
        if (out.meta.size >= 0 && out.meta.size != out.bytesWritten) {
            return Error::make(ErrorCategory::Transient,
                               QStringLiteral("received %1 bytes, expected %2").arg(out.bytesWritten).arg(out.meta.size));
        }
        if (ranged && out.bytesWritten != params.rangeLength
            && !(out.objectSize >= 0 && params.rangeStart + out.bytesWritten == out.objectSize)) {
            return Error::make(ErrorCategory::Protocol,
                               QStringLiteral("received %1 bytes of a %2-byte range").arg(out.bytesWritten).arg(params.rangeLength));
        }
        if (!ranged && params.partNumber <= 0 && etagIsContentMd5(out.meta.etag, r)) {
            QString expected = out.meta.etag;
            expected.remove(QLatin1Char('"'));
            if (expected.toLatin1().toLower() != out.md5.toHex()) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("integrity check failed: MD5 of the received "
                                                  "data does not match the ETag"));
            }
            out.integrity = Integrity::Md5Verified;
        }
        return out;
    });
}

quint64 S3Client::putObject(const PutObjectParams& params, Done<PutObjectResult> done, Progress progress)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    if (params.file) {
        if (!params.data.isEmpty())
            return failLater(std::move(done), invalid(QStringLiteral("either data or a file, not both")));
        if (params.file->path.isEmpty() || params.file->offset < 0 || params.file->length < 0)
            return failLater(std::move(done), invalid(QStringLiteral("invalid file body")));
        if (params.file->md5Base64.isEmpty())
            return failLater(std::move(done), invalid(QStringLiteral("a streamed body needs its Content-MD5")));
    }

    RequestSpec spec;
    spec.operation = Operation::PutObject;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.body = params.data;
    spec.file = params.file;
    spec.progress = std::move(progress);
    spec.contentType = params.contentType.isEmpty() ? QByteArray("application/octet-stream")
                                                    : params.contentType.toUtf8();
    // Retrying a PUT of the same bytes is safe (on versioned buckets it may
    // leave one extra version).
    spec.idempotent = true;
    spec.headers.append({"Content-MD5", params.file ? params.file->md5Base64 : contentMd5(params.data)});
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
        out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        out.throttled = r.throttled;
        return Result<PutObjectResult>(out);
    });
}

quint64 S3Client::copyObject(const CopyObjectParams& params, Done<CopyObjectResult> done)
{
    if (params.sourceBucket.isEmpty() || params.sourceKey.isEmpty() || params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("source or destination is missing")));
    if (params.sourceBucket == params.bucket && params.sourceKey == params.key && params.sourceVersionId.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("an object cannot be copied onto itself")));
    // Same rule as for request paths: QNAM would normalise them in the URL,
    // the server might in the header.
    for (const QString& k : {params.sourceKey, params.key}) {
        const QStringList segments = k.split(u'/');
        if (segments.contains(QStringLiteral(".")) || segments.contains(QStringLiteral("..")))
            return failLater(std::move(done), invalid(QStringLiteral("keys with \".\" or \"..\" segments cannot be copied")));
    }

    RequestSpec spec;
    spec.operation = Operation::CopyObject;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.contentType = "application/octet-stream";   // ignored with the COPY directive
    // A lost answer leaves the outcome open (another version on versioned
    // buckets): never retried blindly — the caller checks with HEAD.
    spec.idempotent = false;
    QByteArray source = '/' + uriEncodeText(params.sourceBucket, SlashMode::Encode) + '/'
                      + uriEncodeText(params.sourceKey, SlashMode::Keep);
    if (!params.sourceVersionId.isEmpty())
        source += "?versionId=" + uriEncodeText(params.sourceVersionId, SlashMode::Encode);
    spec.headers.append({"x-amz-copy-source", source});
    spec.headers.append({"x-amz-metadata-directive", QByteArray("COPY")});
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"x-amz-copy-source-if-match", params.ifMatch.toLatin1()});
    if (!params.storageClass.isEmpty())
        spec.headers.append({"x-amz-storage-class", params.storageClass.toLatin1()});
    // A copy can fail after the 200 has been sent: the body says so.
    spec.mayReturn200Error = true;

    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<CopyObjectResult> {
        Result<CopyObjectResult> parsed = xml::parseCopyObjectResult(r.body);
        if (!parsed)
            return parsed;
        parsed->versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        parsed->sourceVersionId = QString::fromUtf8(r.header("x-amz-copy-source-version-id"));
        parsed->requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        return parsed;
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
        out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
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
        parsed->requestId = QString::fromLatin1(r.header("x-amz-request-id"));
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

// --- Presigning ---------------------------------------------------------------------

quint64 S3Client::presign(const PresignRequest& request, Done<QUrl> done)
{
    const ProviderProfile& profile = *config().profile;
    if (request.expiresSeconds > profile.maxPresignExpirySeconds()) {
        return failLater(std::move(done),
                         invalid(QStringLiteral("%1 allows presigned URLs for at most %2 seconds")
                                     .arg(profile.displayName())
                                     .arg(profile.maxPresignExpirySeconds())));
    }

    const auto op = newOp();
    auto sign = [this, op, request, done = std::move(done)](Result<Credentials> creds) {
        endOp(op);
        if (op->cancelled) {
            done(cancelledError());
            return;
        }
        if (!creds) {
            done(creds.error());
            return;
        }
        const ProviderProfile& profile = *config().profile;
        Endpoint endpoint = m_pipeline->endpointFor(request.bucket);
        const AddressingStyle style = endpoint.addressing == AddressingStyle::Auto
            ? profile.defaultAddressing()
            : endpoint.addressing;
        endpoint.region = profile.signingRegion(endpoint.region);
        done(Presigner::presign(endpoint, style, creds.value(), request, m_pipeline->correctedNow()));
    };
    // Through the event loop: cached credentials would answer synchronously.
    QTimer::singleShot(0, this, [this, sign = std::move(sign)]() mutable {
        m_pipeline->withCredentials(std::move(sign));
    });
    return op->id;
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
