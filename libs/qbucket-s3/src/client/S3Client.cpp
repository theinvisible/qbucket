#include "client/S3Client.h"

#include "http/UriEncoding.h"

#include "core/HttpDate.h"
#include "xml/XmlParsers.h"
#include "xml/XmlWriters.h"

#include <QCryptographicHash>
#include <QSet>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

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
    m.partsCount = r.header("x-amz-mp-parts-count").trimmed().toInt();
    m.cacheControl = QString::fromLatin1(r.header("cache-control"));
    m.contentDisposition = QString::fromUtf8(r.header("content-disposition"));
    m.contentEncoding = QString::fromLatin1(r.header("content-encoding"));
    m.contentLanguage = QString::fromLatin1(r.header("content-language"));
    m.expires = QString::fromLatin1(r.header("expires"));
    m.websiteRedirectLocation = QString::fromUtf8(r.header("x-amz-website-redirect-location"));
    m.serverSideEncryption = QString::fromLatin1(r.header("x-amz-server-side-encryption"));
    m.sseKmsKeyId = QString::fromLatin1(r.header("x-amz-server-side-encryption-aws-kms-key-id"));
    m.sseCustomerAlgorithm = QString::fromLatin1(r.header("x-amz-server-side-encryption-customer-algorithm"));
    if (const QByteArray tags = r.header("x-amz-tagging-count"); !tags.isNull())
        m.tagCount = tags.trimmed().toInt();
    m.objectLockMode = QString::fromLatin1(r.header("x-amz-object-lock-mode"));
    m.objectLockRetainUntil = QDateTime::fromString(QString::fromLatin1(r.header("x-amz-object-lock-retain-until-date")),
                                                    Qt::ISODateWithMs);
    m.legalHold = r.header("x-amz-object-lock-legal-hold").trimmed().compare("ON", Qt::CaseInsensitive) == 0;
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

// x-amz-meta-* headers; printable ASCII only (for now).
std::optional<Error> appendMetadata(RequestSpec& spec, const QMap<QString, QString>& metadata)
{
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) {
        if (it.key().isEmpty() || !isPrintableAscii(it.key()) || !isPrintableAscii(it.value()))
            return invalid(QStringLiteral("metadata must be printable ASCII: ") + it.key());
        spec.headers.append({"x-amz-meta-" + it.key().toLower().toLatin1(), it.value().toLatin1()});
    }
    return std::nullopt;
}

// Content-Type, the other settable headers and x-amz-meta-* of an object.
std::optional<Error> appendObjectHeaders(RequestSpec& spec, const ObjectHeaders& h)
{
    const std::pair<const char*, const QString*> fields[] = {
        {"Content-Type", &h.contentType},
        {"Cache-Control", &h.cacheControl},
        {"Content-Disposition", &h.contentDisposition},
        {"Content-Encoding", &h.contentEncoding},
        {"Content-Language", &h.contentLanguage},
        {"Expires", &h.expires},
        {"x-amz-website-redirect-location", &h.websiteRedirectLocation},
    };
    for (const auto& [name, value] : fields) {
        if (!isPrintableAscii(*value))
            return invalid(QStringLiteral("%1 must be printable ASCII").arg(QLatin1String(name)));
    }
    spec.contentType = h.contentType.isEmpty() ? QByteArray("application/octet-stream") : h.contentType.toLatin1();
    for (const auto& [name, value] : fields) {
        if (qstrcmp(name, "Content-Type") != 0 && !value->isEmpty())
            spec.headers.append({QByteArray(name), value->toLatin1()});
    }
    return appendMetadata(spec, h.userMetadata);
}

// x-amz-copy-source value: /bucket/key[?versionId=…], encoded exactly once.
std::optional<QByteArray> copySource(const QString& bucket, const QString& key, const QString& versionId)
{
    // Same rule as for request paths: QNAM would normalise them in the URL,
    // the server might in the header.
    const QStringList segments = key.split(u'/');
    if (segments.contains(QStringLiteral(".")) || segments.contains(QStringLiteral("..")))
        return std::nullopt;
    QByteArray source = '/' + uriEncodeText(bucket, SlashMode::Encode) + '/' + uriEncodeText(key, SlashMode::Keep);
    if (!versionId.isEmpty())
        source += "?versionId=" + uriEncodeText(versionId, SlashMode::Encode);
    return source;
}

// x-amz-tagging value: "k1=v1&k2=v2", URL-encoded. An empty set is an empty
// header (the copy gets no tags).
QByteArray taggingHeader(const TagSet& tags)
{
    QByteArrayList pairs;
    for (const Tag& t : tags)
        pairs.append(QUrl::toPercentEncoding(t.key) + '=' + QUrl::toPercentEncoding(t.value));
    return pairs.join('&');
}

// PutObjectTagging / PutObjectAcl answer without an ETag. One with an ETag
// was handled as an object upload — a proxy that drops unknown subresources
// does that (Hetzner's turned DELETE ?encryption into DeleteBucket).
std::optional<Error> answeredLikeAnUpload(const RawResponse& r, const QString& what)
{
    if (r.header("etag").isNull())
        return std::nullopt;
    return Error::make(ErrorCategory::Protocol,
                       QStringLiteral("%1 was answered like an object upload (with an ETag): the server may have "
                                      "stored the request as the object's content — check its versions")
                           .arg(what));
}

RequestSpec subresourceSpec(Operation op, const QByteArray& method, const QString& bucket, const QString& key,
                            const QByteArray& subresource, const QString& versionId)
{
    RequestSpec spec;
    spec.operation = op;
    spec.method = method;
    spec.bucket = bucket;
    spec.key = key;
    spec.query.append({subresource, {}, true});
    spec.versionId = versionId;
    if (!versionId.isEmpty())
        spec.query.append({"versionId", versionId.toUtf8(), false});
    return spec;
}

bool validPartNumber(int n)
{
    return n >= 1 && n <= 10000;
}

// A PUT on a bucket subresource answered like CreateBucket — a proxy that
// dropped the subresource: on an existing bucket 409 BucketAlreadyOwnedByYou
// (or 200 with Location in us-east-1), on a missing one a new bucket.
Error createBucketAnswer(Error e, const QString& what)
{
    if (e.category == ErrorCategory::Conflict
        && (e.code == QLatin1String("BucketAlreadyOwnedByYou") || e.code == QLatin1String("BucketAlreadyExists"))) {
        e.category = ErrorCategory::Protocol;
        e.message = QStringLiteral("server answered %1 like CreateBucket (%2): the subresource was dropped")
                        .arg(what, e.code);
    }
    return e;
}

std::optional<Error> answeredLikeCreateBucket(const RawResponse& r, const QString& what)
{
    if (r.header("location").isEmpty())
        return std::nullopt;
    return Error::make(ErrorCategory::Protocol,
                       QStringLiteral("server answered %1 like CreateBucket (Location header): the subresource "
                                      "was dropped")
                           .arg(what));
}

// A DELETE on a bucket subresource answered like DeleteBucket: 409
// BucketNotEmpty (Hetzner's proxy ran DELETE ?encryption as DeleteBucket).
// On an empty bucket DeleteBucket would succeed — the caller checks that
// the bucket is still there.
Error deleteBucketAnswer(Error e, const QString& what)
{
    if (e.category == ErrorCategory::Conflict && e.code == QLatin1String("BucketNotEmpty")) {
        e.category = ErrorCategory::Protocol;
        e.message = QStringLiteral("server answered %1 like DeleteBucket (BucketNotEmpty): the subresource was "
                                   "dropped — check that the bucket still exists")
                        .arg(what);
    }
    return e;
}

bool hasChildElements(const QByteArray& body)
{
    QXmlStreamReader r(body);
    int depth = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && ++depth == 2)
            return true;
        if (r.isEndElement())
            --depth;
    }
    return false;
}

QByteArray isoTimestamp(const QDateTime& t)
{
    return t.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ")).toLatin1();
}

SubresourceWriteResult writeResult(const RawResponse& r)
{
    SubresourceWriteResult out;
    out.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
    out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
    return out;
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
    QList<Feature> needed = featuresFor(spec.operation);
    for (const auto& [name, value] : spec.headers) {
        if (spec.method == "GET" || spec.method == "HEAD")
            break;
        if (name.compare("If-None-Match", Qt::CaseInsensitive) == 0)
            needed.append(Feature::ConditionalWrites);
        else if (name.compare("If-Match", Qt::CaseInsensitive) == 0)
            needed.append(Feature::ConditionalOverwrite);
    }
    if (!spec.sourceBucket.isEmpty() && spec.sourceBucket != spec.bucket)
        needed.append(Feature::CrossBucketCopy);
    // Object writes that set an ACL or tags (bucket ACLs are other features).
    if (!spec.key.isEmpty() && spec.method != "GET" && spec.method != "HEAD") {
        for (const auto& [name, value] : spec.headers) {
            if (name.compare("x-amz-acl", Qt::CaseInsensitive) == 0)
                needed.append(Feature::ObjectAclWrite);
            else if (name.compare("x-amz-tagging", Qt::CaseInsensitive) == 0)
                needed.append(Feature::ObjectTaggingWrite);
        }
    }
    if (needed.isEmpty())
        return std::nullopt;
    for (const Feature f : needed) {
        if (effectiveEntry(f, spec.bucket).state == CapabilityState::Unsupported) {
            Error err = Error::make(ErrorCategory::Unsupported,
                                    QStringLiteral("the server does not support %1").arg(featureId(f)));
            err.resource = spec.bucket;
            err.code = QStringLiteral("CapabilityUnsupported");
            return err;
        }
    }
    return std::nullopt;
}

CapabilityEntry S3Client::effectiveEntry(Feature feature, const QString& bucket) const
{
    const auto bucketIt = m_bucketCapabilities.constFind(bucket);
    CapabilityEntry e = bucketIt != m_bucketCapabilities.constEnd()
        ? capabilities::effective(feature, *bucketIt, m_serverCapabilities)
        : m_serverCapabilities.entry(feature);
    if (e.state == CapabilityState::Unknown)
        e = config().profile->capabilities().entry(feature);
    return e;
}

void S3Client::learnFrom(const RequestSpec& spec, const Error& error)
{
    const QList<Feature> features = featuresFor(spec.operation);
    if (error.category != ErrorCategory::Unsupported || spec.bucket.isEmpty() || features.isEmpty())
        return;
    // Only what the operation stands for: a 501 on PutObjectTagging says
    // nothing about reading tags.
    {
        const Feature f = features.front();
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
    HeadObjectParams params;
    params.bucket = bucket;
    params.key = key;
    params.versionId = versionId;
    return headObject(params, std::move(done));
}

quint64 S3Client::headObject(const HeadObjectParams& params, Done<ObjectMeta> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    if (params.partNumber != 0 && !validPartNumber(params.partNumber))
        return failLater(std::move(done), invalid(QStringLiteral("invalid part number")));
    RequestSpec spec;
    spec.operation = Operation::HeadObject;
    spec.method = "HEAD";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.versionId = params.versionId;
    if (!params.versionId.isEmpty())
        spec.query.append({"versionId", params.versionId.toUtf8(), false});
    if (params.partNumber > 0)
        spec.query.append({"partNumber", QByteArray::number(params.partNumber), false});
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"If-Match", params.ifMatch.toLatin1()});
    return single(std::move(spec), std::move(done), [key = params.key](const RawResponse& r) {
        return Result<ObjectMeta>(metaFromHeaders(key, r));
    });
}

// --- Objects -----------------------------------------------------------------------

quint64 S3Client::getObject(const GetObjectParams& params, Done<ObjectData> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    const bool ranged = params.rangeLength >= 0;
    if (ranged && (params.rangeStart < 0 || params.rangeLength == 0))
        return failLater(std::move(done), invalid(QStringLiteral("invalid byte range")));
    RequestSpec spec;
    spec.operation = Operation::GetObject;
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.versionId = params.versionId;
    if (!params.versionId.isEmpty())
        spec.query.append({"versionId", params.versionId.toUtf8(), false});
    if (ranged) {
        spec.headers.append({"Range", "bytes=" + QByteArray::number(params.rangeStart) + '-'
                                          + QByteArray::number(params.rangeStart + params.rangeLength - 1)});
    }
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"If-Match", params.ifMatch.toLatin1()});
    // Never mayReturn200Error: the object itself may be an <Error> document.
    return single(std::move(spec), std::move(done), [params, ranged](const RawResponse& r) -> Result<ObjectData> {
        ObjectData out;
        out.meta = metaFromHeaders(params.key, r);
        out.data = r.body;
        if (out.meta.size >= 0 && out.meta.size != out.data.size()) {
            return Error::make(ErrorCategory::Transient,
                               QStringLiteral("received %1 bytes, expected %2")
                                   .arg(out.data.size())
                                   .arg(out.meta.size));
        }
        if (ranged) {
            if (r.status != 206) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("the server ignored the Range header (status %1)").arg(r.status));
            }
            // "bytes 0-99/1234": the object's size; "*" when unknown.
            const QByteArray contentRange = r.header("content-range").trimmed();
            const QByteArray expected = "bytes " + QByteArray::number(params.rangeStart) + '-';
            if (!contentRange.startsWith(expected)) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("the server answered another range: ")
                                       + QString::fromLatin1(contentRange));
            }
            bool ok = false;
            const qint64 total = contentRange.mid(contentRange.lastIndexOf('/') + 1).toLongLong(&ok);
            if (ok)
                out.objectSize = total;
            const qint64 received = out.data.size();
            if (received != params.rangeLength && !(ok && params.rangeStart + received == total)) {
                return Error::make(ErrorCategory::Protocol,
                                   QStringLiteral("received %1 bytes of a %2-byte range").arg(received).arg(params.rangeLength));
            }
            return out;
        }
        out.objectSize = out.data.size();
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
    spec.sinkCreate = params.createFile;
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
    if (params.headers && (!params.contentType.isEmpty() || !params.metadata.isEmpty()))
        return failLater(std::move(done), invalid(QStringLiteral("either headers or a content type and metadata, not both")));
    if (params.ifNoneMatch && !params.ifMatch.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("If-Match and If-None-Match exclude each other")));
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
    if (!params.serverSideEncryption.isEmpty())
        spec.headers.append({"x-amz-server-side-encryption", params.serverSideEncryption.toLatin1()});
    if (!params.cannedAcl.isEmpty())
        spec.headers.append({"x-amz-acl", params.cannedAcl.toLatin1()});
    if (params.ifNoneMatch)
        spec.headers.append({"If-None-Match", QByteArray("*")});
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"If-Match", params.ifMatch.toLatin1()});
    if (params.headers) {
        if (std::optional<Error> e = appendObjectHeaders(spec, *params.headers))
            return failLater(std::move(done), *e);
    } else if (std::optional<Error> e = appendMetadata(spec, params.metadata)) {
        return failLater(std::move(done), *e);
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
    const bool replace = params.metadataDirective == MetadataDirective::Replace;
    if (params.sourceBucket == params.bucket && params.sourceKey == params.key && params.sourceVersionId.isEmpty()
        && !replace && params.storageClass.isEmpty() && params.serverSideEncryption.isEmpty()) {
        return failLater(std::move(done), invalid(QStringLiteral("an object cannot be copied onto itself unchanged")));
    }
    const std::optional<QByteArray> source = copySource(params.sourceBucket, params.sourceKey, params.sourceVersionId);
    if (!source)
        return failLater(std::move(done), invalid(QStringLiteral("keys with \".\" or \"..\" segments cannot be copied")));

    RequestSpec spec;
    spec.operation = Operation::CopyObject;
    spec.sourceBucket = params.sourceBucket;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.contentType = "application/octet-stream";   // ignored with the COPY directive
    // A lost answer leaves the outcome open (another version on versioned
    // buckets): never retried blindly — the caller checks with HEAD.
    spec.idempotent = false;
    spec.headers.append({"x-amz-copy-source", *source});
    spec.headers.append({"x-amz-metadata-directive", replace ? QByteArray("REPLACE") : QByteArray("COPY")});
    if (replace) {
        if (std::optional<Error> e = appendObjectHeaders(spec, params.headers))
            return failLater(std::move(done), *e);
    }
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"x-amz-copy-source-if-match", params.ifMatch.toLatin1()});
    if (!params.storageClass.isEmpty())
        spec.headers.append({"x-amz-storage-class", params.storageClass.toLatin1()});
    if (!params.serverSideEncryption.isEmpty())
        spec.headers.append({"x-amz-server-side-encryption", params.serverSideEncryption.toLatin1()});
    if (!params.cannedAcl.isEmpty())
        spec.headers.append({"x-amz-acl", params.cannedAcl.toLatin1()});
    if (params.tags) {
        if (const QString problem = tagSetProblem(*params.tags); !problem.isEmpty())
            return failLater(std::move(done), invalid(problem));
        spec.headers.append({"x-amz-tagging-directive", QByteArray("REPLACE")});
        spec.headers.append({"x-amz-tagging", taggingHeader(*params.tags)});
    } else {
        spec.headers.append({"x-amz-tagging-directive", QByteArray("COPY")});
    }
    // A copy can fail after the 200 has been sent: the body says so.
    spec.mayReturn200Error = true;
    spec.serverWork = true;

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

// --- Tags and ACLs ----------------------------------------------------------------

quint64 S3Client::getObjectTagging(const QString& bucket, const QString& key, const QString& versionId,
                                   Done<ObjectTagging> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec = subresourceSpec(Operation::GetObjectTagging, "GET", bucket, key, "tagging", versionId);
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<ObjectTagging> {
        Result<TagSet> tags = xml::parseTagging(r.body);
        if (!tags)
            return tags.error();
        ObjectTagging out;
        out.tags = *tags;
        out.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        return out;
    });
}

quint64 S3Client::putObjectTagging(const PutObjectTaggingParams& params, Done<SubresourceWriteResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    if (const QString problem = tagSetProblem(params.tags); !problem.isEmpty())
        return failLater(std::move(done), invalid(problem));
    Result<QByteArray> body = xml::buildTagging(params.tags);
    if (!body)
        return failLater(std::move(done), body.error());
    RequestSpec spec = subresourceSpec(Operation::PutObjectTagging, "PUT", params.bucket, params.key, "tagging",
                                       params.versionId);
    spec.body = *body;
    spec.contentType = "application/xml";
    spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    spec.idempotent = true;   // the same set again
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<SubresourceWriteResult> {
        if (std::optional<Error> e = answeredLikeAnUpload(r, QStringLiteral("PutObjectTagging")))
            return *e;
        SubresourceWriteResult out;
        out.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        return out;
    });
}

quint64 S3Client::getObjectAcl(const QString& bucket, const QString& key, const QString& versionId,
                               Done<AccessControlPolicy> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec = subresourceSpec(Operation::GetObjectAcl, "GET", bucket, key, "acl", versionId);
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<AccessControlPolicy> {
        return xml::parseAccessControlPolicy(r.body);
    });
}

quint64 S3Client::putObjectAcl(const PutObjectAclParams& params, Done<SubresourceWriteResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    if (params.cannedAcl.isEmpty() || !isPrintableAscii(params.cannedAcl))
        return failLater(std::move(done), invalid(QStringLiteral("a canned ACL is required")));
    RequestSpec spec = subresourceSpec(Operation::PutObjectAcl, "PUT", params.bucket, params.key, "acl",
                                       params.versionId);
    spec.headers.append({"x-amz-acl", params.cannedAcl.toLatin1()});
    spec.idempotent = true;
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<SubresourceWriteResult> {
        if (std::optional<Error> e = answeredLikeAnUpload(r, QStringLiteral("PutObjectAcl")))
            return *e;
        SubresourceWriteResult out;
        out.versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        return out;
    });
}

// --- Multipart uploads -----------------------------------------------------------

quint64 S3Client::createMultipartUpload(const CreateMultipartUploadParams& params,
                                        Done<CreateMultipartUploadResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec;
    spec.operation = Operation::CreateMultipartUpload;
    spec.method = "POST";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.query.append({"uploads", {}, true});
    // The object's content type and headers are fixed here, not by the parts.
    if (std::optional<Error> e = appendObjectHeaders(spec, params.headers))
        return failLater(std::move(done), *e);
    if (!params.storageClass.isEmpty())
        spec.headers.append({"x-amz-storage-class", params.storageClass.toLatin1()});
    if (!params.serverSideEncryption.isEmpty())
        spec.headers.append({"x-amz-server-side-encryption", params.serverSideEncryption.toLatin1()});
    if (!params.cannedAcl.isEmpty())
        spec.headers.append({"x-amz-acl", params.cannedAcl.toLatin1()});
    if (!params.tagging.isEmpty())
        spec.headers.append({"x-amz-tagging", params.tagging.toLatin1()});
    // A lost answer leaves an orphaned upload behind at worst, which
    // ListMultipartUploads shows and Abort removes — a retry is acceptable.
    spec.idempotent = true;
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<CreateMultipartUploadResult> {
        Result<CreateMultipartUploadResult> parsed = xml::parseInitiateMultipartUpload(r.body);
        if (parsed)
            parsed->requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        return parsed;
    });
}

quint64 S3Client::uploadPart(const UploadPartParams& params, Done<UploadPartResult> done, Progress progress)
{
    if (params.bucket.isEmpty() || params.key.isEmpty() || params.uploadId.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket, key or upload id is missing")));
    if (!validPartNumber(params.partNumber))
        return failLater(std::move(done), invalid(QStringLiteral("part numbers are 1 to 10000")));
    if (params.file.path.isEmpty() || params.file.offset < 0 || params.file.length < 0)
        return failLater(std::move(done), invalid(QStringLiteral("invalid file body")));
    if (params.file.md5Base64.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("a part needs its Content-MD5")));
    RequestSpec spec;
    spec.operation = Operation::UploadPart;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.query.append({"partNumber", QByteArray::number(params.partNumber), false});
    spec.query.append({"uploadId", params.uploadId.toUtf8(), false});
    spec.file = params.file;
    spec.contentType = "application/octet-stream";
    spec.headers.append({"Content-MD5", params.file.md5Base64});
    spec.progress = std::move(progress);
    spec.idempotent = true;   // the same part number again replaces it
    return single(std::move(spec), std::move(done), [n = params.partNumber](const RawResponse& r) -> Result<UploadPartResult> {
        UploadPartResult out;
        out.partNumber = n;
        out.etag = QString::fromLatin1(r.header("etag"));
        out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        out.throttled = r.throttled;
        if (out.etag.isEmpty())
            return Error::make(ErrorCategory::Protocol, QStringLiteral("UploadPart answered without an ETag"));
        return out;
    });
}

quint64 S3Client::completeMultipartUpload(const CompleteMultipartUploadParams& params,
                                          Done<CompleteMultipartUploadResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty() || params.uploadId.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket, key or upload id is missing")));
    if (params.parts.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("a multipart upload needs at least one part")));
    int previous = 0;
    for (const CompletedPart& p : params.parts) {
        if (!validPartNumber(p.partNumber) || p.partNumber <= previous || p.etag.isEmpty())
            return failLater(std::move(done), invalid(QStringLiteral("parts must be listed in ascending order, each with its ETag")));
        previous = p.partNumber;
    }
    RequestSpec spec;
    spec.operation = Operation::CompleteMultipartUpload;
    spec.method = "POST";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.query.append({"uploadId", params.uploadId.toUtf8(), false});
    spec.body = xml::buildCompleteMultipartUpload(params.parts);
    spec.contentType = "application/xml";
    spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    if (params.ifNoneMatch && !params.ifMatch.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("If-Match and If-None-Match exclude each other")));
    if (params.ifNoneMatch)
        spec.headers.append({"If-None-Match", QByteArray("*")});
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"If-Match", params.ifMatch.toLatin1()});
    // The server may fail after sending 200 (assembling takes a while) —
    // and a lost answer leaves it open whether the object now exists.
    spec.mayReturn200Error = true;
    spec.idempotent = false;
    spec.serverWork = true;
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<CompleteMultipartUploadResult> {
        Result<CompleteMultipartUploadResult> parsed = xml::parseCompleteMultipartUpload(r.body);
        if (!parsed)
            return parsed;
        parsed->versionId = QString::fromUtf8(r.header("x-amz-version-id"));
        parsed->requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        return parsed;
    });
}

quint64 S3Client::abortMultipartUpload(const QString& bucket, const QString& key, const QString& uploadId,
                                       Done<void> done)
{
    if (bucket.isEmpty() || key.isEmpty() || uploadId.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket, key or upload id is missing")));
    RequestSpec spec;
    spec.operation = Operation::AbortMultipartUpload;
    spec.method = "DELETE";
    spec.bucket = bucket;
    spec.key = key;
    spec.query.append({"uploadId", uploadId.toUtf8(), false});
    spec.idempotent = true;
    return single(std::move(spec), std::move(done), [](const RawResponse&) { return Result<void>(); });
}

quint64 S3Client::listParts(const ListPartsParams& params, Done<ListPartsPage> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty() || params.uploadId.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket, key or upload id is missing")));
    RequestSpec spec;
    spec.operation = Operation::ListParts;
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.query.append({"uploadId", params.uploadId.toUtf8(), false});
    spec.query.append({"max-parts", QByteArray::number(std::clamp(params.maxParts, 1, 1000)), false});
    if (params.partNumberMarker > 0)
        spec.query.append({"part-number-marker", QByteArray::number(params.partNumberMarker), false});
    return single(std::move(spec), std::move(done), [marker = params.partNumberMarker](const RawResponse& r) -> Result<ListPartsPage> {
        Result<ListPartsPage> page = xml::parseListParts(r.body);
        if (page && page->isTruncated && page->nextPartNumberMarker <= marker) {
            return Error::make(ErrorCategory::Protocol,
                               QStringLiteral("ListParts made no progress (listing loop)"));
        }
        return page;
    });
}

quint64 S3Client::listObjectVersions(const ListObjectVersionsParams& params, Done<ListObjectVersionsPage> done)
{
    if (params.bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    if (params.keyMarker.isEmpty() && !params.versionIdMarker.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("version-id-marker without key-marker")));
    RequestSpec spec;
    spec.operation = Operation::ListObjectVersions;
    spec.bucket = params.bucket;
    spec.query.append({"versions", {}, true});
    // Always url-encoded: keys may hold characters XML can't carry.
    spec.query.append({"encoding-type", "url", false});
    spec.query.append({"max-keys", QByteArray::number(std::clamp(params.maxKeys, 1,
                                                                 config().profile->maxKeysPerList())), false});
    spec.query.append({"prefix", params.prefix.toUtf8(), false});
    if (!params.delimiter.isEmpty())
        spec.query.append({"delimiter", params.delimiter.toUtf8(), false});
    if (!params.keyMarker.isEmpty())
        spec.query.append({"key-marker", params.keyMarker.toUtf8(), false});
    if (!params.versionIdMarker.isEmpty())
        spec.query.append({"version-id-marker", params.versionIdMarker.toUtf8(), false});
    return single(std::move(spec), std::move(done), [this, params](const RawResponse& r) -> Result<ListObjectVersionsPage> {
        Result<ListObjectVersionsPage> page =
            xml::parseListVersions(r.body, config().profile->listingDecodesPlusAsSpace());
        if (!page)
            return page;
        if (page->isTruncated && !params.keyMarker.isEmpty() && page->nextKeyMarker == params.keyMarker
            && page->nextVersionIdMarker == params.versionIdMarker) {
            return Error::make(ErrorCategory::Protocol,
                               QStringLiteral("ListObjectVersions returned the same markers again (listing loop)"));
        }
        if (page->bucket.isEmpty())
            page->bucket = params.bucket;
        return page;
    });
}

quint64 S3Client::getBucketVersioning(const QString& bucket, Done<BucketVersioning> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::GetBucketVersioning;
    spec.bucket = bucket;
    spec.query.append({"versioning", {}, true});
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<BucketVersioning> {
        return xml::parseVersioningConfiguration(r.body);
    });
}

quint64 S3Client::putBucketVersioning(const PutBucketVersioningParams& params, Done<SubresourceWriteResult> done)
{
    if (params.bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    if (params.status == VersioningStatus::Never)
        return failLater(std::move(done), invalid(QStringLiteral("versioning can only be enabled or suspended")));
    RequestSpec spec;
    spec.operation = Operation::PutBucketVersioning;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.query.append({"versioning", {}, true});
    spec.body = xml::buildVersioningConfiguration(params.status);
    spec.contentType = "application/xml";
    spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    spec.idempotent = true;   // the same status again
    // A proxy that drops ?versioning makes this a CreateBucket.
    auto guarded = [done = std::move(done)](Result<SubresourceWriteResult> r) mutable {
        if (!r)
            r = createBucketAnswer(r.error(), QStringLiteral("PutBucketVersioning"));
        done(std::move(r));
    };
    return single(std::move(spec), Done<SubresourceWriteResult>(std::move(guarded)),
                  [](const RawResponse& r) -> Result<SubresourceWriteResult> {
        if (std::optional<Error> e = answeredLikeCreateBucket(r, QStringLiteral("PutBucketVersioning")))
            return *e;
        SubresourceWriteResult out;
        out.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        return out;
    });
}

// --- Bucket configuration -----------------------------------------------------------

quint64 S3Client::getBucketConfig(const QString& bucket, BucketConfig kind, Done<BucketConfigDocument> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec = subresourceSpec(bucketConfigGetOperation(kind), "GET", bucket, {},
                                       bucketConfigSubresource(kind), {});
    auto notConfigured = [kind, done = std::move(done)](Result<BucketConfigDocument> r) mutable {
        if (!r && r.error().httpStatus == 404
            && (bucketConfigNotConfiguredCodes(kind).contains(r.error().code)
                || r.error().category == ErrorCategory::NotConfigured)) {
            BucketConfigDocument doc;
            doc.kind = kind;
            doc.requestId = r.error().requestId;
            r = doc;
        }
        done(std::move(r));
    };
    return single(std::move(spec), Done<BucketConfigDocument>(std::move(notConfigured)),
                  [kind](const RawResponse& r) -> Result<BucketConfigDocument> {
        BucketConfigDocument doc;
        doc.kind = kind;
        doc.body = r.body;
        doc.requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        if (r.status == 204 && r.body.isEmpty())   // Garage: no website
            return doc;
        const QLatin1StringView root = bucketConfigRoot(kind);
        if (root.isEmpty()) {
            const QJsonDocument json = QJsonDocument::fromJson(r.body);
            if (!json.isObject()) {
                Error e = Error::make(ErrorCategory::Unsupported,
                                      QStringLiteral("the answer to Get%1 is no JSON policy: the server ignored "
                                                     "?%2")
                                          .arg(bucketConfigName(kind), QString::fromLatin1(bucketConfigSubresource(kind))));
                e.code = QStringLiteral("NoJson");
                return e;
            }
            doc.configured = !json.object().isEmpty();
            return doc;
        }
        const QString found = xml::rootElementName(r.body);
        if (found != root) {
            Error e = Error::make(ErrorCategory::Unsupported,
                                  QStringLiteral("the answer to Get%1 has the root <%2>, not <%3>: the server "
                                                 "ignored ?%4")
                                      .arg(bucketConfigName(kind), found, QString(root),
                                           QString::fromLatin1(bucketConfigSubresource(kind))));
            e.code = QStringLiteral("Root:") + found;
            return e;
        }
        doc.configured = kind == BucketConfig::Acl || kind == BucketConfig::PolicyStatus
            || hasChildElements(r.body);
        return doc;
    });
}

quint64 S3Client::putBucketConfig(const PutBucketConfigParams& params, Done<SubresourceWriteResult> done)
{
    if (params.bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    const std::optional<Operation> op = bucketConfigPutOperation(params.kind);
    if (!op) {
        return failLater(std::move(done), invalid(QStringLiteral("%1 is read-only in this library")
                                                      .arg(bucketConfigName(params.kind))));
    }
    if (params.kind == BucketConfig::Acl) {
        if (params.cannedAcl.isEmpty() == params.body.isEmpty())
            return failLater(std::move(done), invalid(QStringLiteral("a bucket ACL is a canned ACL or a document")));
        if (!params.cannedAcl.isEmpty() && !isPrintableAscii(params.cannedAcl))
            return failLater(std::move(done), invalid(QStringLiteral("invalid canned ACL")));
    } else if (!params.cannedAcl.isEmpty()) {
        return failLater(std::move(done), invalid(QStringLiteral("a canned ACL is only for the bucket ACL")));
    } else if (params.body.isEmpty()) {
        return failLater(std::move(done), invalid(QStringLiteral("the document is empty")));
    }
    // Only what is a document of this kind leaves the machine.
    if (!params.body.isEmpty()) {
        const QLatin1StringView root = bucketConfigRoot(params.kind);
        const bool ok = root.isEmpty() ? QJsonDocument::fromJson(params.body).isObject()
                                       : xml::rootElementName(params.body) == root;
        if (!ok) {
            return failLater(std::move(done),
                             invalid(root.isEmpty() ? QStringLiteral("the policy is no JSON object")
                                                    : QStringLiteral("the document's root is not <%1>").arg(QString(root))));
        }
    }
    const QString name = operationName(*op);
    RequestSpec spec = subresourceSpec(*op, "PUT", params.bucket, {}, bucketConfigSubresource(params.kind), {});
    if (!params.body.isEmpty()) {
        spec.body = params.body;
        spec.contentType = params.kind == BucketConfig::Policy ? "application/json" : "application/xml";
        spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    } else {
        spec.headers.append({"x-amz-acl", params.cannedAcl.toLatin1()});
    }
    spec.idempotent = true;   // the same document again
    auto guarded = [name, done = std::move(done)](Result<SubresourceWriteResult> r) mutable {
        if (!r)
            r = createBucketAnswer(r.error(), name);
        done(std::move(r));
    };
    return single(std::move(spec), Done<SubresourceWriteResult>(std::move(guarded)),
                  [name](const RawResponse& r) -> Result<SubresourceWriteResult> {
        if (std::optional<Error> e = answeredLikeCreateBucket(r, name))
            return *e;
        return writeResult(r);
    });
}

quint64 S3Client::deleteBucketConfig(const QString& bucket, BucketConfig kind, Done<SubresourceWriteResult> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    const std::optional<Operation> op = bucketConfigDeleteOperation(kind);
    if (!op)
        return failLater(std::move(done), invalid(QStringLiteral("%1 can't be deleted").arg(bucketConfigName(kind))));
    const QString name = operationName(*op);
    RequestSpec spec = subresourceSpec(*op, "DELETE", bucket, {}, bucketConfigSubresource(kind), {});
    spec.idempotent = true;
    auto guarded = [kind, name, done = std::move(done)](Result<SubresourceWriteResult> r) mutable {
        if (!r && r.error().httpStatus == 404 && bucketConfigNotConfiguredCodes(kind).contains(r.error().code)) {
            SubresourceWriteResult out;   // nothing was set (or a retry after a lost answer)
            out.requestId = r.error().requestId;
            r = out;
        } else if (!r) {
            r = deleteBucketAnswer(r.error(), name);
        }
        done(std::move(r));
    };
    return single(std::move(spec), Done<SubresourceWriteResult>(std::move(guarded)),
                  [](const RawResponse& r) -> Result<SubresourceWriteResult> { return writeResult(r); });
}

// --- Object lock of an object ------------------------------------------------------------

quint64 S3Client::getObjectRetention(const QString& bucket, const QString& key, const QString& versionId,
                                     Done<ObjectRetention> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec = subresourceSpec(Operation::GetObjectRetention, "GET", bucket, key, "retention", versionId);
    auto none = [done = std::move(done)](Result<ObjectRetention> r) mutable {
        if (!r && r.error().httpStatus == 404
            && (r.error().code == QLatin1String("NoSuchObjectLockConfiguration")
                || r.error().code == QLatin1String("ObjectLockConfigurationNotFoundError")))
            r = ObjectRetention{};
        done(std::move(r));
    };
    return single(std::move(spec), Done<ObjectRetention>(std::move(none)), [](const RawResponse& r) {
        return xml::parseRetention(r.body);
    });
}

quint64 S3Client::putObjectRetention(const PutObjectRetentionParams& params, Done<SubresourceWriteResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    if (params.mode.has_value() != params.retainUntil.isValid())
        return failLater(std::move(done), invalid(QStringLiteral("a retention has a mode and a date")));
    if (!params.mode && !params.bypassGovernanceRetention) {
        return failLater(std::move(done),
                         invalid(QStringLiteral("removing a retention needs the governance bypass")));
    }
    RequestSpec spec = subresourceSpec(Operation::PutObjectRetention, "PUT", params.bucket, params.key, "retention",
                                       params.versionId);
    spec.body = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                R"(<Retention xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)";
    if (params.mode) {
        spec.body += "<Mode>" + objectLockModeName(*params.mode).toLatin1() + "</Mode><RetainUntilDate>"
                   + isoTimestamp(params.retainUntil) + "</RetainUntilDate>";
    }
    spec.body += "</Retention>";
    spec.contentType = "application/xml";
    spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    if (params.bypassGovernanceRetention)
        spec.headers.emplaceBack("x-amz-bypass-governance-retention", "true");
    spec.idempotent = true;
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<SubresourceWriteResult> {
        if (std::optional<Error> e = answeredLikeAnUpload(r, QStringLiteral("PutObjectRetention")))
            return *e;
        return writeResult(r);
    });
}

quint64 S3Client::getObjectLegalHold(const QString& bucket, const QString& key, const QString& versionId,
                                     Done<bool> done)
{
    if (bucket.isEmpty() || key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec = subresourceSpec(Operation::GetObjectLegalHold, "GET", bucket, key, "legal-hold", versionId);
    auto none = [done = std::move(done)](Result<bool> r) mutable {
        if (!r && r.error().httpStatus == 404
            && (r.error().code == QLatin1String("NoSuchObjectLockConfiguration")
                || r.error().code == QLatin1String("ObjectLockConfigurationNotFoundError")))
            r = false;
        done(std::move(r));
    };
    return single(std::move(spec), Done<bool>(std::move(none)), [](const RawResponse& r) {
        return xml::parseLegalHold(r.body);
    });
}

quint64 S3Client::putObjectLegalHold(const PutObjectLegalHoldParams& params, Done<SubresourceWriteResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec = subresourceSpec(Operation::PutObjectLegalHold, "PUT", params.bucket, params.key,
                                       "legal-hold", params.versionId);
    spec.body = QByteArray(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                           R"(<LegalHold xmlns="http://s3.amazonaws.com/doc/2006-03-01/"><Status>)")
              + (params.on ? "ON" : "OFF") + "</Status></LegalHold>";
    spec.contentType = "application/xml";
    spec.headers.append({"Content-MD5", contentMd5(spec.body)});
    spec.idempotent = true;
    return single(std::move(spec), std::move(done), [](const RawResponse& r) -> Result<SubresourceWriteResult> {
        if (std::optional<Error> e = answeredLikeAnUpload(r, QStringLiteral("PutObjectLegalHold")))
            return *e;
        return writeResult(r);
    });
}

quint64 S3Client::listMultipartUploads(const ListMultipartUploadsParams& params, Done<ListMultipartUploadsPage> done)
{
    if (params.bucket.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket is missing")));
    RequestSpec spec;
    spec.operation = Operation::ListMultipartUploads;
    spec.bucket = params.bucket;
    spec.query.append({"uploads", {}, true});
    spec.query.append({"encoding-type", "url", false});
    spec.query.append({"max-uploads", QByteArray::number(std::clamp(params.maxUploads, 1, 1000)), false});
    if (!params.prefix.isEmpty())
        spec.query.append({"prefix", params.prefix.toUtf8(), false});
    if (!params.keyMarker.isEmpty())
        spec.query.append({"key-marker", params.keyMarker.toUtf8(), false});
    if (!params.uploadIdMarker.isEmpty())
        spec.query.append({"upload-id-marker", params.uploadIdMarker.toUtf8(), false});
    return single(std::move(spec), std::move(done), [this, params](const RawResponse& r) -> Result<ListMultipartUploadsPage> {
        Result<ListMultipartUploadsPage> page =
            xml::parseListMultipartUploads(r.body, config().profile->listingDecodesPlusAsSpace());
        if (page && page->isTruncated && page->nextKeyMarker == params.keyMarker
            && page->nextUploadIdMarker == params.uploadIdMarker) {
            return Error::make(ErrorCategory::Protocol,
                               QStringLiteral("ListMultipartUploads returned the same markers again (listing loop)"));
        }
        return page;
    });
}

quint64 S3Client::uploadPartCopy(const UploadPartCopyParams& params, Done<UploadPartCopyResult> done)
{
    if (params.sourceBucket.isEmpty() || params.sourceKey.isEmpty() || params.bucket.isEmpty() || params.key.isEmpty()
        || params.uploadId.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("source, destination or upload id is missing")));
    if (!validPartNumber(params.partNumber))
        return failLater(std::move(done), invalid(QStringLiteral("part numbers are 1 to 10000")));
    if (params.rangeLength == 0 || (params.rangeLength > 0 && params.rangeStart < 0))
        return failLater(std::move(done), invalid(QStringLiteral("invalid byte range")));
    const std::optional<QByteArray> source = copySource(params.sourceBucket, params.sourceKey, params.sourceVersionId);
    if (!source)
        return failLater(std::move(done), invalid(QStringLiteral("keys with \".\" or \"..\" segments cannot be copied")));

    RequestSpec spec;
    spec.operation = Operation::UploadPartCopy;
    spec.method = "PUT";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.sourceBucket = params.sourceBucket;
    spec.query.append({"partNumber", QByteArray::number(params.partNumber), false});
    spec.query.append({"uploadId", params.uploadId.toUtf8(), false});
    spec.contentType = "application/octet-stream";
    spec.headers.append({"x-amz-copy-source", *source});
    if (params.rangeLength > 0) {
        spec.headers.append({"x-amz-copy-source-range", "bytes=" + QByteArray::number(params.rangeStart) + '-'
                                                            + QByteArray::number(params.rangeStart + params.rangeLength - 1)});
    }
    if (!params.ifMatch.isEmpty())
        spec.headers.append({"x-amz-copy-source-if-match", params.ifMatch.toLatin1()});
    spec.mayReturn200Error = true;
    spec.idempotent = true;   // replaces the part of an unfinished upload
    spec.serverWork = true;
    return single(std::move(spec), std::move(done), [n = params.partNumber](const RawResponse& r) -> Result<UploadPartCopyResult> {
        Result<UploadPartCopyResult> parsed = xml::parseCopyPartResult(r.body);
        if (!parsed)
            return parsed;
        parsed->partNumber = n;
        parsed->requestId = QString::fromLatin1(r.header("x-amz-request-id"));
        parsed->throttled = r.throttled;
        return parsed;
    });
}

quint64 S3Client::deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                               Done<DeleteObjectResult> done)
{
    return deleteObject(DeleteObjectParams{bucket, key, versionId, false}, std::move(done));
}

quint64 S3Client::deleteObject(const DeleteObjectParams& params, Done<DeleteObjectResult> done)
{
    if (params.bucket.isEmpty() || params.key.isEmpty())
        return failLater(std::move(done), invalid(QStringLiteral("bucket or key is missing")));
    RequestSpec spec;
    spec.operation = Operation::DeleteObject;
    spec.method = "DELETE";
    spec.bucket = params.bucket;
    spec.key = params.key;
    spec.versionId = params.versionId;
    if (!params.versionId.isEmpty())
        spec.query.append({"versionId", params.versionId.toUtf8(), false});
    if (params.bypassGovernanceRetention)
        spec.headers.emplaceBack("x-amz-bypass-governance-retention", "true");
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
    return deleteObjects(DeleteObjectsParams{bucket, objects, false}, std::move(done));
}

quint64 S3Client::deleteObjects(const DeleteObjectsParams& params, Done<DeleteObjectsResult> done)
{
    const QString& bucket = params.bucket;
    const QList<ObjectIdentifier>& objects = params.objects;
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
    if (params.bypassGovernanceRetention)
        spec.headers.emplaceBack("x-amz-bypass-governance-retention", "true");

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
    if (effectiveEntry(Feature::Presign, request.bucket).state == CapabilityState::Unsupported) {
        Error err = Error::make(ErrorCategory::Unsupported, QStringLiteral("the server does not support object.presign"));
        err.resource = request.bucket;
        err.code = QStringLiteral("CapabilityUnsupported");
        return failLater(std::move(done), err);
    }
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
