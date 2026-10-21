#include <qbucket/s3/testing/FakeObjectStore.h>

#include <qbucket/s3/BucketConfig.h>

#include "core/HttpDate.h"
#include "http/UriEncoding.h"
#include "xml/XmlParsers.h"

#include <QCryptographicHash>
#include <QHash>
#include <QXmlStreamReader>

#include <algorithm>
#include <tuple>

namespace qbucket::s3::testing {
namespace {

QByteArray md5(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5);
}

QString quoted(const QByteArray& rawMd5)
{
    return QLatin1Char('"') + QString::fromLatin1(rawMd5.toHex()) + QLatin1Char('"');
}

QString unquoted(QString etag)
{
    etag.remove(QLatin1Char('"'));
    return etag;
}

QByteArray xmlEscape(const QString& text)
{
    QByteArray out;
    for (const char c : text.toUtf8()) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

QByteArray iso(const QDateTime& t)
{
    return t.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ")).toLatin1();
}

QString decode(const QByteArray& raw)
{
    QByteArray out;
    if (!uriDecode(raw, false, &out))
        return {};
    return QString::fromUtf8(out);
}

FakeResponse error(int status, const QByteArray& code, const QByteArray& message)
{
    return FakeResponse::s3Error(status, code, message);
}

FakeResponse xmlResponse(const QByteArray& body)
{
    return FakeResponse::ok("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + body, "application/xml");
}

// "bytes=a-b", "bytes=a-", "bytes=-n" → [start, end] (inclusive), clamped.
std::optional<std::pair<qint64, qint64>> parseRange(const QByteArray& header, qint64 size, bool* invalid)
{
    *invalid = false;
    if (!header.startsWith("bytes="))
        return std::nullopt;
    const QByteArray spec = header.mid(6);
    const qsizetype dash = spec.indexOf('-');
    if (dash < 0 || spec.contains(',')) {
        *invalid = true;
        return std::nullopt;
    }
    const QByteArray a = spec.left(dash);
    const QByteArray b = spec.mid(dash + 1);
    qint64 start = 0;
    qint64 end = size - 1;
    if (a.isEmpty()) {
        start = std::max<qint64>(0, size - b.toLongLong());
    } else {
        start = a.toLongLong();
        if (!b.isEmpty())
            end = std::min(size - 1, b.toLongLong());
    }
    if (start >= size || start > end) {
        *invalid = true;
        return std::nullopt;
    }
    return std::pair{start, end};
}

struct Part {
    QByteArray data;
    qint64 size = 0;
    QByteArray md5;
    QString etag;
};

struct Upload {
    QString bucket;
    QString key;
    QString id;
    QDateTime initiated;
    QString contentType;
    QString storageClass;
    QMap<QString, QString> metadata;
    QMap<QString, QString> headers;
    QString acl;
    TagSet tags;
    std::map<int, Part> parts;
};

QMap<QString, QString> userMetadata(const RecordedRequest& req)
{
    QMap<QString, QString> out;
    for (const auto& [name, value] : req.headers) {
        const QByteArray lower = name.toLower();
        if (lower.startsWith("x-amz-meta-"))
            out.insert(QString::fromLatin1(lower.mid(11)), QString::fromUtf8(value));
    }
    return out;
}

constexpr const char* kStoredHeaders[] = {"cache-control", "content-disposition", "content-encoding",
                                          "content-language", "expires", "x-amz-website-redirect-location",
                                          "x-amz-server-side-encryption",
                                          "x-amz-server-side-encryption-aws-kms-key-id",
                                          "x-amz-server-side-encryption-customer-algorithm",
                                          "x-amz-object-lock-mode", "x-amz-object-lock-retain-until-date",
                                          "x-amz-object-lock-legal-hold"};

// The stored headers a request sets (see Object::headers).
QMap<QString, QString> storedHeaders(const RecordedRequest& req)
{
    QMap<QString, QString> out;
    for (const char* name : kStoredHeaders) {
        const QByteArray value = req.header(name);
        if (!value.isNull())
            out.insert(QString::fromLatin1(name), QString::fromUtf8(value));
    }
    return out;
}

// x-amz-tagging: "k1=v1&k2=v2", URL-encoded.
TagSet taggingHeader(const RecordedRequest& req)
{
    TagSet out;
    const QByteArray raw = req.header("x-amz-tagging");
    if (raw.isEmpty())
        return out;
    for (const QByteArray& pair : raw.split('&')) {
        const qsizetype eq = pair.indexOf('=');
        const QByteArray k = eq < 0 ? pair : pair.left(eq);
        const QByteArray v = eq < 0 ? QByteArray() : pair.mid(eq + 1);
        out.append({QUrl::fromPercentEncoding(QByteArray(k).replace('+', ' ')),
                    QUrl::fromPercentEncoding(QByteArray(v).replace('+', ' '))});
    }
    return out;
}

// An <AccessControlPolicy> for a canned ACL.
QByteArray aclDocument(const QString& acl)
{
    QByteArray body = "<AccessControlPolicy><Owner><ID>fake-owner</ID><DisplayName>owner</DisplayName></Owner>"
                      "<AccessControlList>"
                      "<Grant><Grantee xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                      "xsi:type=\"CanonicalUser\"><ID>fake-owner</ID></Grantee>"
                      "<Permission>FULL_CONTROL</Permission></Grant>";
    const auto group = [&body](const char* name, const char* permission) {
        body += QByteArray("<Grant><Grantee xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                           "xsi:type=\"Group\"><URI>http://acs.amazonaws.com/groups/global/")
              + name + "</URI></Grantee><Permission>" + permission + "</Permission></Grant>";
    };
    if (acl == QLatin1String("public-read") || acl == QLatin1String("public-read-write"))
        group("AllUsers", "READ");
    if (acl == QLatin1String("public-read-write"))
        group("AllUsers", "WRITE");
    if (acl == QLatin1String("authenticated-read"))
        group("AuthenticatedUsers", "READ");
    return body + "</AccessControlList></AccessControlPolicy>";
}

constexpr const char* kLockMode = "x-amz-object-lock-mode";
constexpr const char* kLockUntil = "x-amz-object-lock-retain-until-date";
constexpr const char* kLegalHold = "x-amz-object-lock-legal-hold";

QString cannedAcl(const RecordedRequest& req)
{
    const QByteArray acl = req.header("x-amz-acl");
    return acl.isNull() ? QStringLiteral("private") : QString::fromLatin1(acl);
}

} // namespace

struct FakeObjectStore::Impl {
    struct BucketData {
        std::map<QByteArray, std::vector<Version>> keys;   // UTF-8 byte order; newest first
        QString versioning;                                 // "", "Enabled", "Suspended"
        bool mfaDelete = false;
        std::map<QByteArray, QByteArray> configs;           // subresource → document
        QString acl = QStringLiteral("private");            // canned, or "custom"
        bool objectLock = false;                            // only at creation
        QByteArray lockConfig;                              // PUT ?object-lock, as sent
    };

    QPointer<FakeS3Server> server;
    std::map<QString, BucketData> buckets;
    std::map<QString, Upload> uploads;                          // by id (sortable)
    QList<QPair<QString, int>> partUploads;
    QHash<QString, int> counts;
    QList<Fault> faults;
    QList<int> faultHits;   // matches per fault
    QHash<QString, int> delays;
    int nextUpload = 1;
    int nextVersion = 1;
    bool discard = false;
    qint64 minPartSize = Q_INT64_C(5) * 1024 * 1024;
    bool honourIfNoneMatch = true;
    bool honourIfMatch = true;
    bool ignoreRanges = false;
    bool ignoreMetadataReplace = false;
    bool dropObjectSubresources = false;
    bool dropTagsOnReplace = false;
    bool interceptSubresourceWrites = false;
    QList<BucketConfig> unsupportedConfigs;

    static std::optional<BucketConfig> configOf(const RecordedRequest& req)
    {
        for (const BucketConfig kind : allBucketConfigs()) {
            if (req.hasQueryItem(bucketConfigSubresource(kind)))
                return kind;
        }
        return std::nullopt;
    }

    // Why this version can't be deleted (or its retention shortened); empty
    // when it can. A legal hold and an active COMPLIANCE retention always
    // protect it, an active GOVERNANCE retention without the bypass.
    static QByteArray protectedBy(const Version& v, bool bypass)
    {
        if (v.locked)
            return "object lock";
        const QMap<QString, QString>& h = v.object.headers;
        if (h.value(QLatin1String(kLegalHold)) == QLatin1String("ON"))
            return "a legal hold";
        const QString mode = h.value(QLatin1String(kLockMode));
        const QDateTime until = xml::parseIsoTimestamp(h.value(QLatin1String(kLockUntil)));
        if (mode.isEmpty() || !until.isValid() || until <= QDateTime::currentDateTimeUtc())
            return {};
        if (mode == QLatin1String("COMPLIANCE"))
            return "a COMPLIANCE retention";
        return bypass ? QByteArray() : QByteArray("a GOVERNANCE retention");
    }

    // The object lock headers of a new version: what the request asks for,
    // else the bucket's default retention.
    void applyLock(const QString& bucket, Object* o, const QMap<QString, QString>& requested)
    {
        for (const char* name : {kLockMode, kLockUntil, kLegalHold})
            o->headers.remove(QLatin1String(name));
        const BucketData& b = buckets[bucket];
        if (!b.objectLock)
            return;
        for (const char* name : {kLockMode, kLockUntil, kLegalHold}) {
            if (requested.contains(QLatin1String(name)))
                o->headers.insert(QLatin1String(name), requested.value(QLatin1String(name)));
        }
        if (o->headers.contains(QLatin1String(kLockMode)) || b.lockConfig.isEmpty())
            return;
        const Result<ObjectLockConfiguration> config = parseObjectLockConfiguration(b.lockConfig);
        if (!config || !config->mode)
            return;
        QDateTime until = QDateTime::currentDateTimeUtc();
        until = config->days ? until.addDays(*config->days) : until.addYears(config->years.value_or(0));
        o->headers.insert(QLatin1String(kLockMode), objectLockModeName(*config->mode));
        o->headers.insert(QLatin1String(kLockUntil), QString::fromLatin1(iso(until)));
    }

    std::vector<Version>* history(const QString& bucket, const QString& key)
    {
        auto b = buckets.find(bucket);
        if (b == buckets.end())
            return nullptr;
        auto h = b->second.keys.find(key.toUtf8());
        return h == b->second.keys.end() || h->second.empty() ? nullptr : &h->second;
    }

    Version* findVersion(const QString& bucket, const QString& key, const QString& versionId)
    {
        std::vector<Version>* h = history(bucket, key);
        if (!h)
            return nullptr;
        for (Version& v : *h) {
            if (v.versionId == versionId)
                return &v;
        }
        return nullptr;
    }

    // The current object; none when the newest entry is a delete marker.
    Object* find(const QString& bucket, const QString& key)
    {
        std::vector<Version>* h = history(bucket, key);
        return h && !h->front().deleteMarker ? &h->front().object : nullptr;
    }

    bool versioned(const QString& bucket) const
    {
        const auto b = buckets.find(bucket);
        return b != buckets.end() && !b->second.versioning.isEmpty();
    }

    // The entry a write or a delete adds: a new version id while versioning
    // is enabled, else "null" — replacing the earlier "null" entry.
    QString addEntry(const QString& bucket, const QString& key, Version v)
    {
        BucketData& b = buckets[bucket];
        std::vector<Version>& h = b.keys[key.toUtf8()];
        if (b.versioning == QLatin1String("Enabled")) {
            v.versionId = QStringLiteral("v%1").arg(nextVersion++, 8, 10, QLatin1Char('0'));
        } else {
            v.versionId = QStringLiteral("null");
            std::erase_if(h, [](const Version& e) { return e.versionId == QLatin1String("null"); });
        }
        h.insert(h.begin(), std::move(v));
        return h.front().versionId;
    }

    QString store(const QString& bucket, const QString& key, const Object& o)
    {
        Version v;
        v.object = o;
        return addEntry(bucket, key, std::move(v));
    }

    // DeleteObject without a version: a delete marker when versioned, else
    // the object is gone. Returns the marker's id (empty without one).
    QString deleteCurrent(const QString& bucket, const QString& key)
    {
        if (!versioned(bucket)) {
            buckets[bucket].keys.erase(key.toUtf8());
            return {};
        }
        Version m;
        m.deleteMarker = true;
        m.object.lastModified = QDateTime::currentDateTimeUtc();
        return addEntry(bucket, key, std::move(m));
    }

    struct VersionDelete {
        bool found = false;
        bool locked = false;
        bool marker = false;
    };

    VersionDelete deleteVersion(const QString& bucket, const QString& key, const QString& versionId,
                                bool bypass = false)
    {
        VersionDelete out;
        std::vector<Version>* h = history(bucket, key);
        if (!h)
            return out;
        const auto it = std::find_if(h->begin(), h->end(), [&](const Version& v) { return v.versionId == versionId; });
        if (it == h->end())
            return out;
        out.found = true;
        out.locked = !protectedBy(*it, bypass).isEmpty();
        out.marker = it->deleteMarker;
        if (out.locked)
            return out;
        h->erase(it);
        if (h->empty())
            buckets[bucket].keys.erase(key.toUtf8());
        return out;
    }

    FakeResponse withVersion(FakeResponse r, const QString& bucket, const QString& versionId)
    {
        if (versioned(bucket) && !versionId.isEmpty())
            r.withHeader("x-amz-version-id", versionId.toLatin1());
        return r;
    }

    QString operationOf(const RecordedRequest& req, const QString& key) const
    {
        const QByteArray& m = req.method;
        if (req.path == "/")
            return QStringLiteral("ListBuckets");
        if (key.isEmpty()) {
            if (m == "GET" && req.hasQueryItem("uploads"))
                return QStringLiteral("ListMultipartUploads");
            if (m == "GET" && req.hasQueryItem("versions"))
                return QStringLiteral("ListObjectVersions");
            const std::optional<BucketConfig> kind = configOf(req);
            if (interceptSubresourceWrites && (kind || req.hasQueryItem("versioning"))) {
                // A proxy that drops the subresource.
                if (m == "PUT")
                    return QStringLiteral("CreateBucket");
                if (m == "DELETE")
                    return QStringLiteral("DeleteBucket");
            }
            if (m == "PUT" && req.hasQueryItem("versioning"))
                return QStringLiteral("PutBucketVersioning");
            if (m == "PUT" && req.query.isEmpty())
                return QStringLiteral("CreateBucket");
            if (m == "DELETE" && req.query.isEmpty())
                return QStringLiteral("DeleteBucket");
            if (kind) {
                const std::optional<Operation> put = bucketConfigPutOperation(*kind);
                const std::optional<Operation> del = bucketConfigDeleteOperation(*kind);
                if (m == "GET")
                    return operationName(bucketConfigGetOperation(*kind));
                if (m == "PUT" && put)
                    return operationName(*put);
                if (m == "DELETE" && del)
                    return operationName(*del);
                return QStringLiteral("Unsupported");
            }
            if (m == "POST" && req.hasQueryItem("delete"))
                return QStringLiteral("DeleteObjects");
            if (m == "HEAD")
                return QStringLiteral("HeadBucket");
            if (m == "GET" && (req.query.isEmpty() || req.hasQueryItem("list-type") || req.hasQueryItem("prefix")))
                return QStringLiteral("ListObjectsV2");
            if (m == "GET")
                return QStringLiteral("GetBucketSubresource");
            return QStringLiteral("Unsupported");
        }
        if (!dropObjectSubresources) {
            if (req.hasQueryItem("tagging") && (m == "GET" || m == "PUT"))
                return m == "GET" ? QStringLiteral("GetObjectTagging") : QStringLiteral("PutObjectTagging");
            if (req.hasQueryItem("acl") && (m == "GET" || m == "PUT"))
                return m == "GET" ? QStringLiteral("GetObjectAcl") : QStringLiteral("PutObjectAcl");
        }
        if (req.hasQueryItem("retention") && (m == "GET" || m == "PUT"))
            return m == "GET" ? QStringLiteral("GetObjectRetention") : QStringLiteral("PutObjectRetention");
        if (req.hasQueryItem("legal-hold") && (m == "GET" || m == "PUT"))
            return m == "GET" ? QStringLiteral("GetObjectLegalHold") : QStringLiteral("PutObjectLegalHold");
        if (m == "POST" && req.hasQueryItem("uploads"))
            return QStringLiteral("CreateMultipartUpload");
        if (m == "POST" && req.hasQueryItem("uploadId"))
            return QStringLiteral("CompleteMultipartUpload");
        if (m == "PUT" && req.hasQueryItem("uploadId"))
            return req.header("x-amz-copy-source").isNull() ? QStringLiteral("UploadPart") : QStringLiteral("UploadPartCopy");
        // Never let an unknown subresource fall through to an upload. A
        // presigned request's X-Amz-* and response-* parameters are no
        // subresource.
        bool onlySigning = true;
        for (const QByteArray& item : req.query.split('&')) {
            if (!item.isEmpty() && !item.startsWith("X-Amz-") && !item.startsWith("response-"))
                onlySigning = false;
        }
        const bool plain = onlySigning || (dropObjectSubresources && (req.hasQueryItem("tagging") || req.hasQueryItem("acl")));
        if (m == "PUT" && plain)
            return req.header("x-amz-copy-source").isNull() ? QStringLiteral("PutObject") : QStringLiteral("CopyObject");
        if (m == "PUT")
            return QStringLiteral("Unsupported");
        if (m == "GET" && req.hasQueryItem("uploadId"))
            return QStringLiteral("ListParts");
        if (m == "GET" && (plain || req.hasQueryItem("partNumber") || req.hasQueryItem("versionId")))
            return QStringLiteral("GetObject");
        if (m == "GET")
            return QStringLiteral("GetObjectSubresource");
        if (m == "HEAD")
            return QStringLiteral("HeadObject");
        if (m == "DELETE" && req.hasQueryItem("uploadId"))
            return QStringLiteral("AbortMultipartUpload");
        if (m == "DELETE" && (req.query.isEmpty() || req.hasQueryItem("versionId")))
            return QStringLiteral("DeleteObject");
        return QStringLiteral("Unsupported");
    }

    // The first fault that applies to this request, counting the match.
    const Fault* faultFor(const QString& op, int partNumber, bool after)
    {
        for (qsizetype i = 0; i < faults.size(); ++i) {
            const Fault& f = faults[i];
            if (f.operation != op || f.afterExecuting != after)
                continue;
            if (f.partNumber != 0 && f.partNumber != partNumber)
                continue;
            const int hit = faultHits[i]++;
            if (hit < f.skip)
                continue;
            if (f.times >= 0 && hit - f.skip >= f.times)
                continue;
            return &f;
        }
        return nullptr;
    }

    FakeResponse handle(const RecordedRequest& req)
    {
        QString bucket;
        QString key;
        const QByteArray path = req.path.mid(1);
        const qsizetype slash = path.indexOf('/');
        bucket = decode(slash < 0 ? path : path.left(slash));
        key = slash < 0 ? QString() : decode(path.mid(slash + 1));

        const QString op = operationOf(req, key);
        ++counts[op];
        const int partNumber = req.queryItem("partNumber").toInt();
        if (const Fault* f = faultFor(op, partNumber, false))
            return f->response;
        FakeResponse response = execute(op, req, bucket, key);
        if (const Fault* f = faultFor(op, partNumber, true))
            return f->response;
        response.delayMs = std::max(response.delayMs, delays.value(op));
        return response;
    }

    FakeResponse execute(const QString& op, const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        if (op == QLatin1String("ListBuckets"))
            return listBuckets();
        if (op == QLatin1String("CreateBucket"))
            return createBucketOp(req, bucket);
        if (buckets.find(bucket) == buckets.end())
            return error(404, "NoSuchBucket", "The specified bucket does not exist");
        if (op == QLatin1String("DeleteBucket"))
            return deleteBucketOp(bucket);
        if (const std::optional<BucketConfig> kind = configOf(req); kind && key.isEmpty() && op != QLatin1String("Unsupported"))
            return bucketConfig(req, bucket, *kind);
        if (op.endsWith(QLatin1String("ObjectRetention")) || op.endsWith(QLatin1String("ObjectLegalHold")))
            return objectLock(op, req, bucket, key);
        if (op == QLatin1String("HeadBucket"))
            return FakeResponse::ok();
        if (op == QLatin1String("ListObjectsV2"))
            return listObjects(req, bucket);
        if (op == QLatin1String("ListMultipartUploads"))
            return listUploads(req, bucket);
        if (op == QLatin1String("ListObjectVersions"))
            return listVersions(req, bucket);
        if (op == QLatin1String("PutBucketVersioning"))
            return putVersioning(req, bucket);
        if (op == QLatin1String("DeleteObjects"))
            return deleteObjects(req, bucket);
        if (op == QLatin1String("GetBucketSubresource")) {
            if (req.hasQueryItem("versioning")) {
                const BucketData& b = buckets[bucket];
                if (b.versioning.isEmpty())
                    return xmlResponse("<VersioningConfiguration/>");
                return xmlResponse("<VersioningConfiguration><Status>" + b.versioning.toLatin1() + "</Status><MfaDelete>"
                                   + QByteArray(b.mfaDelete ? "Enabled" : "Disabled")
                                   + "</MfaDelete></VersioningConfiguration>");
            }
            return error(501, "NotImplemented", "not implemented by FakeObjectStore");
        }
        if (op == QLatin1String("PutObject"))
            return putObject(req, bucket, key);
        if (op == QLatin1String("CopyObject"))
            return copyObject(req, bucket, key);
        if (op == QLatin1String("GetObject") || op == QLatin1String("HeadObject"))
            return getObject(req, bucket, key, op == QLatin1String("HeadObject"));
        if (op == QLatin1String("DeleteObject"))
            return deleteObject(req, bucket, key);
        if (op == QLatin1String("CreateMultipartUpload"))
            return createUpload(req, bucket, key);
        if (op == QLatin1String("UploadPart") || op == QLatin1String("UploadPartCopy"))
            return uploadPart(req, bucket, key, op == QLatin1String("UploadPartCopy"));
        if (op == QLatin1String("CompleteMultipartUpload"))
            return complete(req, bucket, key);
        if (op == QLatin1String("AbortMultipartUpload")) {
            const auto it = uploads.find(decode(req.queryItem("uploadId")));
            if (it == uploads.end() || it->second.bucket != bucket || it->second.key != key)
                return error(404, "NoSuchUpload", "The specified upload does not exist");
            uploads.erase(it);
            FakeResponse r;
            r.status = 204;
            return r;
        }
        if (op == QLatin1String("ListParts"))
            return listParts(req, bucket, key);
        if (op == QLatin1String("GetObjectTagging") || op == QLatin1String("PutObjectTagging")
            || op == QLatin1String("GetObjectAcl") || op == QLatin1String("PutObjectAcl"))
            return subresource(op, req, bucket, key);
        return error(501, "NotImplemented", "not implemented by FakeObjectStore");
    }

    FakeResponse subresource(const QString& op, const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        Object* o = nullptr;
        if (req.hasQueryItem("versionId")) {
            Version* v = findVersion(bucket, key, decode(req.queryItem("versionId")));
            if (!v)
                return error(404, "NoSuchVersion", "The specified version does not exist.");
            if (v->deleteMarker)
                return markerAnswer(error(405, "MethodNotAllowed", "The specified method is not allowed against this resource."), *v);
            o = &v->object;
        } else {
            o = find(bucket, key);
        }
        if (!o)
            return error(404, "NoSuchKey", "The specified key does not exist.");
        if (op == QLatin1String("GetObjectTagging")) {
            QByteArray body = "<Tagging><TagSet>";
            for (const Tag& t : o->tags)
                body += "<Tag><Key>" + xmlEscape(t.key) + "</Key><Value>" + xmlEscape(t.value) + "</Value></Tag>";
            return xmlResponse(body + "</TagSet></Tagging>");
        }
        if (op == QLatin1String("PutObjectTagging")) {
            if (std::optional<FakeResponse> bad = checkMd5(req))
                return *bad;
            Result<TagSet> tags = xml::parseTagging(req.body);
            if (!tags)
                return error(400, "MalformedXML", "The XML you provided was not well-formed");
            o->tags = *tags;
            return FakeResponse::ok();
        }
        if (op == QLatin1String("GetObjectAcl"))
            return xmlResponse(aclDocument(o->acl));
        // PutObjectAcl (canned only)
        const QByteArray acl = req.header("x-amz-acl");
        if (acl.isEmpty())
            return error(400, "MissingSecurityHeader", "x-amz-acl is required by FakeObjectStore");
        o->acl = QString::fromLatin1(acl);
        return FakeResponse::ok();
    }

    FakeResponse createBucketOp(const RecordedRequest& req, const QString& bucket)
    {
        if (buckets.find(bucket) != buckets.end())
            return error(409, "BucketAlreadyOwnedByYou", "Your previous request to create the named bucket succeeded and you already own it.");
        if (bucketNameProblem(bucket) != BucketNameProblem::None)
            return error(400, "InvalidBucketName", "The specified bucket is not valid.");
        BucketData& b = buckets[bucket];
        if (req.header("x-amz-bucket-object-lock-enabled") == "true") {
            b.objectLock = true;
            b.versioning = QStringLiteral("Enabled");
        }
        return FakeResponse::ok().withHeader("Location", "/" + bucket.toUtf8());
    }

    FakeResponse deleteBucketOp(const QString& bucket)
    {
        const BucketData& b = buckets[bucket];
        const bool hasUploads = std::any_of(uploads.begin(), uploads.end(),
                                            [&](const auto& u) { return u.second.bucket == bucket; });
        if (!b.keys.empty() || hasUploads)
            return error(409, "BucketNotEmpty", "The bucket you tried to delete is not empty");
        buckets.erase(bucket);
        FakeResponse r;
        r.status = 204;
        return r;
    }

    FakeResponse bucketConfig(const RecordedRequest& req, const QString& bucket, BucketConfig kind)
    {
        if (unsupportedConfigs.contains(kind))
            return error(501, "NotImplemented", "not implemented by FakeObjectStore");
        BucketData& b = buckets[bucket];
        const QByteArray sub = bucketConfigSubresource(kind);
        const QByteArray notConfigured = bucketConfigNotConfiguredCodes(kind).value(0).toLatin1();
        if (req.method == "GET") {
            const auto it = b.configs.find(sub);
            switch (kind) {
            case BucketConfig::Acl:
                return xmlResponse(aclDocument(b.acl));
            case BucketConfig::PolicyStatus: {
                const auto policy = b.configs.find("policy");
                if (policy == b.configs.end())
                    return error(404, "NoSuchBucketPolicy", "The bucket policy does not exist");
                return xmlResponse("<PolicyStatus><IsPublic>"
                                   + QByteArray(analyzePolicy(policy->second).publicRead ? "true" : "false")
                                   + "</IsPublic></PolicyStatus>");
            }
            case BucketConfig::ObjectLock:
                if (!b.objectLock)
                    return error(404, "ObjectLockConfigurationNotFoundError", "Object Lock configuration does not exist for this bucket");
                return b.lockConfig.isEmpty()
                    ? xmlResponse("<ObjectLockConfiguration><ObjectLockEnabled>Enabled</ObjectLockEnabled></ObjectLockConfiguration>")
                    : FakeResponse::ok(b.lockConfig, "application/xml");
            case BucketConfig::Logging:
                return it == b.configs.end() ? xmlResponse("<BucketLoggingStatus/>") : FakeResponse::ok(it->second, "application/xml");
            case BucketConfig::Notifications:
                return it == b.configs.end() ? xmlResponse("<NotificationConfiguration/>")
                                             : FakeResponse::ok(it->second, "application/xml");
            default:
                if (it == b.configs.end())
                    return error(404, notConfigured, "The configuration does not exist");
                return FakeResponse::ok(it->second, kind == BucketConfig::Policy ? "application/json" : "application/xml");
            }
        }
        if (req.method == "DELETE") {
            b.configs.erase(sub);
            FakeResponse r;
            r.status = 204;
            return r;
        }
        // PUT
        if (!req.body.isEmpty() && req.header("content-md5").isNull())
            return error(400, "InvalidRequest", "Missing required header for this request: Content-MD5");
        if (std::optional<FakeResponse> bad = checkMd5(req))
            return *bad;
        switch (kind) {
        case BucketConfig::Policy:
            if (!policyProblems(req.body).isEmpty())
                return error(400, "MalformedPolicy", "Policies must be valid JSON and the first byte must be '{'");
            break;
        case BucketConfig::Cors: {
            const Result<CorsConfiguration> c = parseCorsConfiguration(req.body);
            if (!c || !corsProblem(*c).isEmpty())
                return error(400, "MalformedXML", "The XML you provided was not well-formed");
            break;
        }
        case BucketConfig::Lifecycle: {
            const Result<LifecycleConfiguration> c = parseLifecycleConfiguration(req.body);
            if (!c || !lifecycleProblem(*c).isEmpty())
                return error(400, "MalformedXML", "The XML you provided was not well-formed");
            break;
        }
        case BucketConfig::PublicAccessBlock:
            if (!parsePublicAccessBlock(req.body))
                return error(400, "MalformedXML", "The XML you provided was not well-formed");
            break;
        case BucketConfig::ObjectLock: {
            const Result<ObjectLockConfiguration> c = parseObjectLockConfiguration(req.body);
            if (!c || !c->enabled)
                return error(400, "MalformedXML", "The XML you provided was not well-formed");
            if (!b.objectLock)
                return error(409, "InvalidBucketState", "Object Lock configuration cannot be enabled on existing buckets");
            if (!objectLockProblem(*c).isEmpty())
                return error(400, "InvalidArgument", "Default retention period must be a positive integer value");
            b.lockConfig = req.body;
            return FakeResponse::ok();
        }
        case BucketConfig::Acl:
            if (!req.body.isEmpty()) {
                const Result<AccessControlPolicy> acl = xml::parseAccessControlPolicy(req.body);
                if (!acl)
                    return error(400, "MalformedACLError", "The XML you provided was not well-formed");
                b.acl = cannedAclOf(*acl).value_or(QStringLiteral("custom"));
            } else {
                b.acl = QString::fromLatin1(req.header("x-amz-acl"));
            }
            return FakeResponse::ok();
        default:
            return error(501, "NotImplemented", "not implemented by FakeObjectStore");
        }
        b.configs[sub] = req.body;
        return FakeResponse::ok();
    }

    FakeResponse objectLock(const QString& op, const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        Version* v = nullptr;
        if (req.hasQueryItem("versionId")) {
            v = findVersion(bucket, key, decode(req.queryItem("versionId")));
            if (!v)
                return error(404, "NoSuchVersion", "The specified version does not exist.");
        } else if (std::vector<Version>* h = history(bucket, key)) {
            v = &h->front();
        }
        if (!v || (v->deleteMarker && !req.hasQueryItem("versionId")))
            return error(404, "NoSuchKey", "The specified key does not exist.");
        if (v->deleteMarker)
            return error(405, "MethodNotAllowed", "The specified method is not allowed against this resource.");
        if (!buckets[bucket].objectLock)
            return error(400, "InvalidRequest", "Bucket is missing Object Lock Configuration");
        QMap<QString, QString>& h = v->object.headers;
        const QString mode = h.value(QLatin1String(kLockMode));
        const QDateTime until = xml::parseIsoTimestamp(h.value(QLatin1String(kLockUntil)));
        if (op == QLatin1String("GetObjectRetention")) {
            if (mode.isEmpty())
                return error(404, "NoSuchObjectLockConfiguration", "The specified object does not have a ObjectLock configuration");
            return xmlResponse("<Retention><Mode>" + mode.toLatin1() + "</Mode><RetainUntilDate>" + iso(until)
                               + "</RetainUntilDate></Retention>");
        }
        if (op == QLatin1String("GetObjectLegalHold")) {
            return xmlResponse("<LegalHold><Status>"
                               + QByteArray(h.value(QLatin1String(kLegalHold)) == QLatin1String("ON") ? "ON" : "OFF")
                               + "</Status></LegalHold>");
        }
        if (req.header("content-md5").isNull())
            return error(400, "InvalidRequest", "Missing required header for this request: Content-MD5");
        if (std::optional<FakeResponse> bad = checkMd5(req))
            return *bad;
        if (op == QLatin1String("PutObjectLegalHold")) {
            const Result<bool> on = xml::parseLegalHold(req.body);
            if (!on)
                return error(400, "MalformedXML", "The XML you provided was not well-formed");
            if (*on)
                h.insert(QLatin1String(kLegalHold), QStringLiteral("ON"));
            else
                h.remove(QLatin1String(kLegalHold));
            return FakeResponse::ok();
        }
        // PutObjectRetention
        const Result<ObjectRetention> wanted = xml::parseRetention(req.body);
        if (!wanted)
            return error(400, "MalformedXML", "The XML you provided was not well-formed");
        const bool active = !mode.isEmpty() && until.isValid() && until > QDateTime::currentDateTimeUtc();
        const bool weaker = !wanted->mode || wanted->retainUntil < until;
        if (active && mode == QLatin1String("COMPLIANCE")
            && (weaker || wanted->mode != ObjectLockMode::Compliance))
            return error(403, "AccessDenied", "Access Denied because object protected by object lock.");
        if (active && mode == QLatin1String("GOVERNANCE") && weaker
            && req.header("x-amz-bypass-governance-retention") != "true")
            return error(403, "AccessDenied", "Access Denied because object protected by object lock.");
        if (const std::optional<ObjectLockMode> newMode = wanted->mode) {
            h.insert(QLatin1String(kLockMode), objectLockModeName(*newMode));
            h.insert(QLatin1String(kLockUntil), QString::fromLatin1(iso(wanted->retainUntil)));
        } else {
            h.remove(QLatin1String(kLockMode));
            h.remove(QLatin1String(kLockUntil));
        }
        return FakeResponse::ok();
    }

    FakeResponse listBuckets()
    {
        QByteArray body = "<ListAllMyBucketsResult><Owner><ID>fake</ID></Owner><Buckets>";
        for (const auto& [name, objects] : buckets)
            body += "<Bucket><Name>" + xmlEscape(name) + "</Name><CreationDate>2026-01-01T00:00:00.000Z</CreationDate></Bucket>";
        return xmlResponse(body + "</Buckets></ListAllMyBucketsResult>");
    }

    // Checks Content-MD5 when present (S3 semantics).
    static std::optional<FakeResponse> checkMd5(const RecordedRequest& req)
    {
        const QByteArray header = req.header("content-md5");
        if (header.isNull())
            return std::nullopt;
        if (QByteArray::fromBase64(header).size() != 16)
            return error(400, "InvalidDigest", "The Content-MD5 you specified was invalid.");
        if (md5(req.body).toBase64() != header)
            return error(400, "BadDigest", "The Content-MD5 you specified did not match what we received.");
        return std::nullopt;
    }

    bool blockedByIfNoneMatch(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        return honourIfNoneMatch && req.header("if-none-match") == "*" && find(bucket, key);
    }

    // If-Match on a write: the current object must have that ETag (S3: 404
    // when there is none, 412 when it differs).
    std::optional<FakeResponse> blockedByIfMatch(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        const QByteArray ifMatch = req.header("if-match");
        if (!honourIfMatch || ifMatch.isNull())
            return std::nullopt;
        const Object* o = find(bucket, key);
        if (!o)
            return error(404, "NoSuchKey", "The specified key does not exist.");
        if (unquoted(QString::fromLatin1(ifMatch)) != unquoted(o->etag))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        return std::nullopt;
    }

    FakeResponse stored(const Object& o, const QString& bucket, const QString& versionId)
    {
        return withVersion(FakeResponse::ok().withHeader("ETag", o.etag.toLatin1()), bucket, versionId);
    }

    static FakeResponse markerAnswer(FakeResponse r, const Version& marker)
    {
        r.withHeader("x-amz-delete-marker", "true");
        r.withHeader("x-amz-version-id", marker.versionId.toLatin1());
        return r;
    }

    FakeResponse putObject(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        if (std::optional<FakeResponse> bad = checkMd5(req))
            return *bad;
        if (blockedByIfNoneMatch(req, bucket, key))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        if (std::optional<FakeResponse> blocked = blockedByIfMatch(req, bucket, key))
            return *blocked;
        Object o;
        o.size = req.body.size();
        o.md5 = md5(req.body);
        if (!discard)
            o.data = req.body;
        o.etag = quoted(o.md5);
        o.lastModified = QDateTime::currentDateTimeUtc();
        o.contentType = QString::fromUtf8(req.header("content-type"));
        o.storageClass = QString::fromLatin1(req.header("x-amz-storage-class"));
        o.metadata = userMetadata(req);
        o.headers = storedHeaders(req);
        o.acl = cannedAcl(req);
        o.tags = taggingHeader(req);
        if (!buckets[bucket].objectLock && !req.header(kLockMode).isNull())
            return error(400, "InvalidRequest", "Bucket is missing Object Lock Configuration");
        applyLock(bucket, &o, storedHeaders(req));
        const QString versionId = store(bucket, key, o);
        return stored(o, bucket, versionId);
    }

    struct Source {
        QString bucket;
        const Object* object = nullptr;
        QString versionId;            // as asked for ("" without ?versionId=)
        QString foundVersionId;       // of the version copied
        std::optional<FakeResponse> failure;
    };

    // x-amz-copy-source → the source object (a version with ?versionId=).
    Source copySource(const RecordedRequest& req)
    {
        Source out;
        QByteArray raw = req.header("x-amz-copy-source");
        QByteArray query;
        if (const qsizetype q = raw.indexOf('?'); q >= 0) {
            query = raw.mid(q + 1);
            raw = raw.left(q);
        }
        if (raw.startsWith('/'))
            raw = raw.mid(1);
        const qsizetype slash = raw.indexOf('/');
        if (slash <= 0) {
            out.failure = error(400, "InvalidArgument", "Copy Source must mention the source bucket and key");
            return out;
        }
        out.bucket = decode(raw.left(slash));
        const QString key = decode(raw.mid(slash + 1));
        if (query.startsWith("versionId=")) {
            out.versionId = decode(query.mid(10));
            const Version* v = findVersion(out.bucket, key, out.versionId);
            if (!v) {
                out.failure = error(404, "NoSuchVersion", "The specified version does not exist.");
                return out;
            }
            if (v->deleteMarker) {
                out.failure = error(400, "InvalidRequest",
                                    "The source of a copy request may not specifically refer to a delete marker by version id.");
                return out;
            }
            out.object = &v->object;
            out.foundVersionId = v->versionId;
            return out;
        }
        out.object = find(out.bucket, key);
        if (!out.object) {
            out.failure = error(404, "NoSuchKey", "The specified key does not exist.");
            return out;
        }
        out.foundVersionId = history(out.bucket, key)->front().versionId;
        return out;
    }

    FakeResponse copyObject(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        const Source source = copySource(req);
        if (source.failure)
            return *source.failure;
        const QByteArray ifMatch = req.header("x-amz-copy-source-if-match");
        if (!ifMatch.isNull() && unquoted(QString::fromLatin1(ifMatch)) != unquoted(source.object->etag))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        const bool replace = req.header("x-amz-metadata-directive").compare("REPLACE", Qt::CaseInsensitive) == 0;
        const QString storageClass = QString::fromLatin1(req.header("x-amz-storage-class"));
        const bool ontoItself = source.bucket == bucket && source.object == find(bucket, key);
        // AWS refuses a copy onto itself that changes nothing.
        if (ontoItself && !replace && storageClass.isEmpty()
            && req.header("x-amz-server-side-encryption").isNull()) {
            return error(400, "InvalidRequest",
                         "This copy request is illegal because it is trying to copy an object to itself without "
                         "changing the object's metadata, storage class, website redirect location or encryption "
                         "attributes.");
        }
        Object copy = *source.object;
        copy.lastModified = QDateTime::currentDateTimeUtc();
        copy.storageClass = storageClass;
        // The encryption comes from the request (or the bucket default), not the source.
        copy.headers.remove(QStringLiteral("x-amz-server-side-encryption"));
        if (replace && !ignoreMetadataReplace) {
            copy.contentType = QString::fromUtf8(req.header("content-type"));
            copy.metadata = userMetadata(req);
            copy.headers = storedHeaders(req);
            if (dropTagsOnReplace && ontoItself)
                copy.tags.clear();
        } else if (const QByteArray sse = req.header("x-amz-server-side-encryption"); !sse.isNull()) {
            copy.headers.insert(QStringLiteral("x-amz-server-side-encryption"), QString::fromLatin1(sse));
        }
        copy.acl = cannedAcl(req);   // never copied
        applyLock(bucket, &copy, storedHeaders(req));   // retention is never copied either
        if (req.header("x-amz-tagging-directive").compare("REPLACE", Qt::CaseInsensitive) == 0)
            copy.tags = taggingHeader(req);
        const QString versionId = store(bucket, key, copy);
        FakeResponse r = withVersion(xmlResponse("<CopyObjectResult><LastModified>" + iso(copy.lastModified)
                                                 + "</LastModified><ETag>" + xmlEscape(copy.etag)
                                                 + "</ETag></CopyObjectResult>"),
                                     bucket, versionId);
        if (versioned(source.bucket))
            r.withHeader("x-amz-copy-source-version-id", source.foundVersionId.toLatin1());
        return r;
    }

    FakeResponse getObject(const RecordedRequest& req, const QString& bucket, const QString& key, bool head)
    {
        const Version* version = nullptr;
        if (req.hasQueryItem("versionId")) {
            version = findVersion(bucket, key, decode(req.queryItem("versionId")));
            if (!version)
                return head ? error(404, "", "") : error(404, "NoSuchVersion", "The specified version does not exist.");
            if (version->deleteMarker) {
                return markerAnswer(head ? error(405, "", "")
                                         : error(405, "MethodNotAllowed",
                                                 "The specified method is not allowed against this resource."),
                                    *version);
            }
        } else if (const std::vector<Version>* h = history(bucket, key)) {
            version = &h->front();
            if (version->deleteMarker)
                return markerAnswer(head ? error(404, "", "") : error(404, "NoSuchKey", "The specified key does not exist."),
                                    *version);
        }
        if (!version)
            return head ? error(404, "", "") : error(404, "NoSuchKey", "The specified key does not exist.");
        const Object* o = &version->object;
        const QByteArray ifMatch = req.header("if-match");
        if (!ifMatch.isNull() && unquoted(QString::fromLatin1(ifMatch)) != unquoted(o->etag))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        if ((discard || (o->data.isEmpty() && o->size > 0)) && !head)
            return error(501, "NotImplemented", "FakeObjectStore keeps no data for this object");

        qint64 start = 0;
        qint64 end = o->size - 1;
        bool partial = false;
        FakeResponse r;
        const int partNumber = req.queryItem("partNumber").toInt();
        if (partNumber > 0) {
            if (o->partSizes.isEmpty()) {
                if (partNumber != 1)
                    return error(416, "InvalidPartNumber", "The requested partnumber is not satisfiable");
            } else {
                if (partNumber > o->partSizes.size())
                    return error(416, "InvalidPartNumber", "The requested partnumber is not satisfiable");
                start = 0;
                for (int i = 0; i < partNumber - 1; ++i)
                    start += o->partSizes[i];
                end = start + o->partSizes[partNumber - 1] - 1;
                partial = true;
                r.withHeader("x-amz-mp-parts-count", QByteArray::number(o->partSizes.size()));
            }
        } else if (!req.header("range").isNull() && !ignoreRanges) {
            bool invalid = false;
            const auto range = parseRange(req.header("range"), o->size, &invalid);
            if (invalid)
                return error(416, "InvalidRange", "The requested range is not satisfiable");
            if (range) {
                start = range->first;
                end = range->second;
                partial = true;
            }
        }
        r.status = partial ? 206 : 200;
        if (partial) {
            r.withHeader("Content-Range", "bytes " + QByteArray::number(start) + '-' + QByteArray::number(end) + '/'
                                              + QByteArray::number(o->size));
        }
        r.withHeader("ETag", o->etag.toLatin1());
        r.withHeader("Last-Modified", formatHttpDate(o->lastModified));
        if (versioned(bucket))
            r.withHeader("x-amz-version-id", version->versionId.toLatin1());
        if (req.hasQueryItem("response-content-disposition"))
            r.withHeader("Content-Disposition", QUrl::fromPercentEncoding(req.queryItem("response-content-disposition")).toUtf8());
        r.withHeader("Content-Type", o->contentType.isEmpty() ? QByteArray("binary/octet-stream") : o->contentType.toUtf8());
        if (!o->storageClass.isEmpty())
            r.withHeader("x-amz-storage-class", o->storageClass.toLatin1());
        for (auto it = o->metadata.cbegin(); it != o->metadata.cend(); ++it)
            r.withHeader("x-amz-meta-" + it.key().toLatin1(), it.value().toUtf8());
        for (auto it = o->headers.cbegin(); it != o->headers.cend(); ++it)
            r.withHeader(it.key().toLatin1(), it.value().toUtf8());
        if (!o->tags.isEmpty())
            r.withHeader("x-amz-tagging-count", QByteArray::number(o->tags.size()));
        const qint64 length = end - start + 1;
        if (head)
            r.declaredContentLength = std::max<qint64>(0, length);
        else
            r.body = o->data.mid(start, std::max<qint64>(0, length));
        return r;
    }

    FakeResponse createUpload(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        Upload u;
        u.bucket = bucket;
        u.key = key;
        u.id = QStringLiteral("upload%1").arg(nextUpload++, 6, 10, QLatin1Char('0'));
        u.initiated = QDateTime::currentDateTimeUtc();
        u.contentType = QString::fromUtf8(req.header("content-type"));
        u.storageClass = QString::fromLatin1(req.header("x-amz-storage-class"));
        u.metadata = userMetadata(req);
        u.headers = storedHeaders(req);
        u.acl = cannedAcl(req);
        u.tags = taggingHeader(req);
        uploads.emplace(u.id, u);
        return xmlResponse("<InitiateMultipartUploadResult><Bucket>" + xmlEscape(bucket) + "</Bucket><Key>"
                           + xmlEscape(key) + "</Key><UploadId>" + u.id.toLatin1()
                           + "</UploadId></InitiateMultipartUploadResult>");
    }

    Upload* uploadFor(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        const auto it = uploads.find(decode(req.queryItem("uploadId")));
        if (it == uploads.end() || it->second.bucket != bucket || it->second.key != key)
            return nullptr;
        return &it->second;
    }

    FakeResponse uploadPart(const RecordedRequest& req, const QString& bucket, const QString& key, bool copy)
    {
        Upload* u = uploadFor(req, bucket, key);
        if (!u)
            return error(404, "NoSuchUpload", "The specified upload does not exist");
        const int number = req.queryItem("partNumber").toInt();
        if (number < 1 || number > 10000)
            return error(400, "InvalidArgument", "Part number must be an integer between 1 and 10000");
        Part p;
        if (copy) {
            const Source source = copySource(req);
            if (source.failure)
                return *source.failure;
            const Object& src = *source.object;
            const QByteArray ifMatch = req.header("x-amz-copy-source-if-match");
            if (!ifMatch.isNull() && unquoted(QString::fromLatin1(ifMatch)) != unquoted(src.etag))
                return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
            qint64 start = 0;
            qint64 end = src.size - 1;
            if (!req.header("x-amz-copy-source-range").isNull()) {
                bool invalid = false;
                const auto range = parseRange(req.header("x-amz-copy-source-range"), src.size, &invalid);
                if (invalid || !range)
                    return error(400, "InvalidArgument", "The x-amz-copy-source-range value must be of the form bytes=first-last");
                start = range->first;
                end = range->second;
            }
            p.size = end - start + 1;
            if (discard || src.data.isEmpty()) {
                p.md5 = md5(src.etag.toLatin1() + QByteArray::number(start) + '-' + QByteArray::number(end));
            } else {
                p.data = src.data.mid(start, p.size);
                p.md5 = md5(p.data);
            }
        } else {
            if (std::optional<FakeResponse> bad = checkMd5(req))
                return *bad;
            p.size = req.body.size();
            p.md5 = md5(req.body);
            if (!discard)
                p.data = req.body;
        }
        p.etag = quoted(p.md5);
        u->parts[number] = p;
        partUploads.append({u->id, number});
        if (copy) {
            return xmlResponse("<CopyPartResult><LastModified>" + iso(QDateTime::currentDateTimeUtc())
                               + "</LastModified><ETag>" + xmlEscape(p.etag) + "</ETag></CopyPartResult>");
        }
        return FakeResponse::ok().withHeader("ETag", p.etag.toLatin1());
    }

    FakeResponse complete(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        Upload* u = uploadFor(req, bucket, key);
        if (!u)
            return error(404, "NoSuchUpload", "The specified upload does not exist");
        QList<QPair<int, QString>> listed;
        QXmlStreamReader r(req.body);
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("Part")) {
                int number = 0;
                QString etag;
                while (r.readNextStartElement()) {
                    if (r.name() == QLatin1String("PartNumber"))
                        number = r.readElementText().toInt();
                    else if (r.name() == QLatin1String("ETag"))
                        etag = r.readElementText();
                    else
                        r.skipCurrentElement();
                }
                listed.append({number, etag});
            }
        }
        if (r.hasError() || listed.isEmpty())
            return error(400, "MalformedXML", "The XML you provided was not well-formed");
        for (qsizetype i = 1; i < listed.size(); ++i) {
            if (listed[i].first <= listed[i - 1].first)
                return error(400, "InvalidPartOrder", "The list of parts was not in ascending order.");
        }
        QList<QByteArray> md5s;
        QList<qint64> sizes;
        QByteArray data;
        qint64 total = 0;
        for (qsizetype i = 0; i < listed.size(); ++i) {
            const auto it = u->parts.find(listed[i].first);
            if (it == u->parts.end() || unquoted(listed[i].second) != unquoted(it->second.etag))
                return error(400, "InvalidPart", "One or more of the specified parts could not be found.");
            if (i + 1 < listed.size() && it->second.size < minPartSize)
                return error(400, "EntityTooSmall", "Your proposed upload is smaller than the minimum allowed object size.");
            md5s.append(it->second.md5);
            sizes.append(it->second.size);
            total += it->second.size;
            if (!discard)
                data += it->second.data;
        }
        if (blockedByIfNoneMatch(req, bucket, key))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        if (std::optional<FakeResponse> blocked = blockedByIfMatch(req, bucket, key))
            return *blocked;
        Object o;
        o.size = total;
        o.data = data;
        if (!discard)
            o.md5 = md5(data);
        o.etag = compositeEtag(md5s);
        o.lastModified = QDateTime::currentDateTimeUtc();
        o.contentType = u->contentType;
        o.storageClass = u->storageClass;
        o.metadata = u->metadata;
        o.headers = u->headers;
        o.acl = u->acl;
        o.tags = u->tags;
        o.partSizes = sizes;
        applyLock(bucket, &o, u->headers);
        const QString versionId = store(bucket, key, o);
        uploads.erase(u->id);
        return withVersion(xmlResponse("<CompleteMultipartUploadResult><Location>http://fake/" + xmlEscape(bucket) + '/'
                           + xmlEscape(key) + "</Location><Bucket>" + xmlEscape(bucket) + "</Bucket><Key>"
                           + xmlEscape(key) + "</Key><ETag>" + xmlEscape(o.etag)
                           + "</ETag></CompleteMultipartUploadResult>"),
                           bucket, versionId);
    }

    FakeResponse listParts(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        Upload* u = uploadFor(req, bucket, key);
        if (!u)
            return error(404, "NoSuchUpload", "The specified upload does not exist");
        const int marker = req.queryItem("part-number-marker").toInt();
        const int maxParts = req.hasQueryItem("max-parts") ? std::clamp(req.queryItem("max-parts").toInt(), 1, 1000) : 1000;
        QByteArray body = "<ListPartsResult><Bucket>" + xmlEscape(bucket) + "</Bucket><Key>" + xmlEscape(key)
                        + "</Key><UploadId>" + u->id.toLatin1() + "</UploadId><PartNumberMarker>"
                        + QByteArray::number(marker) + "</PartNumberMarker><MaxParts>" + QByteArray::number(maxParts)
                        + "</MaxParts>";
        int returned = 0;
        int last = marker;
        bool truncated = false;
        for (auto it = u->parts.upper_bound(marker); it != u->parts.end(); ++it) {
            if (returned == maxParts) {
                truncated = true;
                break;
            }
            body += "<Part><PartNumber>" + QByteArray::number(it->first) + "</PartNumber><LastModified>"
                  + iso(QDateTime::currentDateTimeUtc()) + "</LastModified><ETag>" + xmlEscape(it->second.etag)
                  + "</ETag><Size>" + QByteArray::number(it->second.size) + "</Size></Part>";
            last = it->first;
            ++returned;
        }
        body += "<IsTruncated>" + QByteArray(truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated)
            body += "<NextPartNumberMarker>" + QByteArray::number(last) + "</NextPartNumberMarker>";
        return xmlResponse(body + "</ListPartsResult>");
    }

    FakeResponse listUploads(const RecordedRequest& req, const QString& bucket)
    {
        const QString prefix = decode(req.queryItem("prefix"));
        const QString keyMarker = decode(req.queryItem("key-marker"));
        const QString idMarker = decode(req.queryItem("upload-id-marker"));
        const int maxUploads = req.hasQueryItem("max-uploads") ? std::clamp(req.queryItem("max-uploads").toInt(), 1, 1000) : 1000;
        const bool encode = req.queryItem("encoding-type") == "url";

        QList<const Upload*> all;
        for (const auto& [id, u] : uploads) {
            if (u.bucket == bucket && u.key.startsWith(prefix))
                all.append(&u);
        }
        std::sort(all.begin(), all.end(), [](const Upload* a, const Upload* b) {
            return std::tuple(a->key.toUtf8(), a->id) < std::tuple(b->key.toUtf8(), b->id);
        });
        QByteArray body = "<ListMultipartUploadsResult><Bucket>" + xmlEscape(bucket) + "</Bucket>";
        if (encode)
            body += "<EncodingType>url</EncodingType>";
        int returned = 0;
        bool truncated = false;
        const Upload* last = nullptr;
        for (const Upload* u : std::as_const(all)) {
            if (!keyMarker.isEmpty()) {
                const QByteArray k = u->key.toUtf8();
                const QByteArray m = keyMarker.toUtf8();
                if (k < m || (k == m && (idMarker.isEmpty() || u->id <= idMarker)))
                    continue;
            }
            if (returned == maxUploads) {
                truncated = true;
                break;
            }
            const QByteArray k = encode ? uriEncodeText(u->key, SlashMode::Keep) : xmlEscape(u->key);
            body += "<Upload><Key>" + k + "</Key><UploadId>" + u->id.toLatin1()
                  + "</UploadId><Initiator><ID>fake</ID><DisplayName>fake-user</DisplayName></Initiator>"
                  + "<StorageClass>" + (u->storageClass.isEmpty() ? QByteArray("STANDARD") : u->storageClass.toLatin1())
                  + "</StorageClass><Initiated>" + iso(u->initiated) + "</Initiated></Upload>";
            last = u;
            ++returned;
        }
        body += "<IsTruncated>" + QByteArray(truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated && last) {
            body += "<NextKeyMarker>" + (encode ? uriEncodeText(last->key, SlashMode::Keep) : xmlEscape(last->key))
                  + "</NextKeyMarker><NextUploadIdMarker>" + last->id.toLatin1() + "</NextUploadIdMarker>";
        }
        return xmlResponse(body + "</ListMultipartUploadsResult>");
    }

    FakeResponse listObjects(const RecordedRequest& req, const QString& bucket)
    {
        const QString prefix = decode(req.queryItem("prefix"));
        const QString delimiter = decode(req.queryItem("delimiter"));
        const int maxKeys = req.hasQueryItem("max-keys") ? std::clamp(req.queryItem("max-keys").toInt(), 1, 1000) : 1000;
        const bool encode = req.queryItem("encoding-type") == "url";
        QByteArray after = decode(req.queryItem("start-after")).toUtf8();
        if (req.hasQueryItem("continuation-token"))
            after = QByteArray::fromBase64(decode(req.queryItem("continuation-token")).toLatin1());
        const auto enc = [encode](const QString& text) {
            return encode ? uriEncodeText(text, SlashMode::Keep) : xmlEscape(text);
        };

        const auto& objects = buckets[bucket].keys;
        const QByteArray p = prefix.toUtf8();
        QByteArray body = "<ListBucketResult><Name>" + xmlEscape(bucket) + "</Name><Prefix>" + enc(prefix)
                        + "</Prefix><MaxKeys>" + QByteArray::number(maxKeys) + "</MaxKeys>";
        if (!delimiter.isEmpty())
            body += "<Delimiter>" + enc(delimiter) + "</Delimiter>";
        if (encode)
            body += "<EncodingType>url</EncodingType>";
        QByteArray contents;
        QByteArray prefixes;
        int returned = 0;
        bool truncated = false;
        QByteArray lastReturned;
        auto it = after.isEmpty() ? objects.lower_bound(p) : objects.upper_bound(after);
        // Continuing after a common prefix: nothing under it again.
        if (!delimiter.isEmpty() && after.endsWith(delimiter.toUtf8())) {
            while (it != objects.end() && it->first.startsWith(after))
                ++it;
        }
        while (it != objects.end()) {
            const QByteArray& k = it->first;
            if (!k.startsWith(p))
                break;
            if (it->second.empty() || it->second.front().deleteMarker) {   // deleted: not listed
                ++it;
                continue;
            }
            QByteArray common;
            if (!delimiter.isEmpty()) {
                const qsizetype at = k.indexOf(delimiter.toUtf8(), p.size());
                if (at >= 0)
                    common = k.left(at + delimiter.toUtf8().size());
            }
            if (returned == maxKeys) {
                truncated = true;
                break;
            }
            if (!common.isEmpty()) {
                prefixes += "<CommonPrefixes><Prefix>" + enc(QString::fromUtf8(common)) + "</Prefix></CommonPrefixes>";
                lastReturned = common;
                ++returned;
                while (it != objects.end() && it->first.startsWith(common))   // everything under it
                    ++it;
                continue;
            }
            const Object& o = it->second.front().object;
            contents += "<Contents><Key>" + enc(QString::fromUtf8(k)) + "</Key><LastModified>" + iso(o.lastModified)
                      + "</LastModified><ETag>" + xmlEscape(o.etag) + "</ETag><Size>" + QByteArray::number(o.size)
                      + "</Size><StorageClass>"
                      + (o.storageClass.isEmpty() ? QByteArray("STANDARD") : o.storageClass.toLatin1())
                      + "</StorageClass></Contents>";
            lastReturned = k;
            ++returned;
            ++it;
        }
        body += "<KeyCount>" + QByteArray::number(returned) + "</KeyCount><IsTruncated>"
              + QByteArray(truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated)
            body += "<NextContinuationToken>" + lastReturned.toBase64() + "</NextContinuationToken>";
        return xmlResponse(body + contents + prefixes + "</ListBucketResult>");
    }

    FakeResponse deleteObjects(const RecordedRequest& req, const QString& bucket)
    {
        QList<QPair<QString, std::optional<QString>>> objects;   // key, version id
        QXmlStreamReader r(req.body);
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("Object")) {
                QString key;
                std::optional<QString> versionId;
                while (r.readNextStartElement()) {
                    if (r.name() == QLatin1String("Key"))
                        key = r.readElementText();
                    else if (r.name() == QLatin1String("VersionId"))
                        versionId = r.readElementText();
                    else
                        r.skipCurrentElement();
                }
                objects.append({key, versionId});
            }
        }
        if (r.hasError())
            return error(400, "MalformedXML", "The XML you provided was not well-formed");
        const bool bypass = req.header("x-amz-bypass-governance-retention") == "true";
        QByteArray body = "<DeleteResult>";
        for (const auto& [key, versionId] : std::as_const(objects)) {
            const QByteArray k = "<Key>" + xmlEscape(key) + "</Key>";
            if (versionId) {
                const VersionDelete del = deleteVersion(bucket, key, *versionId, bypass);
                const QByteArray v = "<VersionId>" + xmlEscape(*versionId) + "</VersionId>";
                if (del.locked) {
                    body += "<Error>" + k + v + "<Code>AccessDenied</Code><Message>Access Denied because object "
                                              "protected by object lock.</Message></Error>";
                    continue;
                }
                body += "<Deleted>" + k + v;
                if (del.marker)
                    body += "<DeleteMarker>true</DeleteMarker><DeleteMarkerVersionId>" + xmlEscape(*versionId)
                          + "</DeleteMarkerVersionId>";
                body += "</Deleted>";
                continue;
            }
            const QString marker = deleteCurrent(bucket, key);
            body += "<Deleted>" + k;
            if (!marker.isEmpty())
                body += "<DeleteMarker>true</DeleteMarker><DeleteMarkerVersionId>" + marker.toLatin1()
                      + "</DeleteMarkerVersionId>";
            body += "</Deleted>";
        }
        return xmlResponse(body + "</DeleteResult>");
    }

    FakeResponse deleteObject(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        FakeResponse r;
        r.status = 204;
        if (req.hasQueryItem("versionId")) {
            const QString versionId = decode(req.queryItem("versionId"));
            const VersionDelete del = deleteVersion(bucket, key, versionId,
                                                    req.header("x-amz-bypass-governance-retention") == "true");
            if (del.locked)
                return error(403, "AccessDenied", "Access Denied because object protected by object lock.");
            if (del.found) {
                r.withHeader("x-amz-version-id", versionId.toLatin1());
                if (del.marker)
                    r.withHeader("x-amz-delete-marker", "true");
            }
            return r;
        }
        const QString marker = deleteCurrent(bucket, key);
        if (!marker.isEmpty()) {
            r.withHeader("x-amz-delete-marker", "true");
            r.withHeader("x-amz-version-id", marker.toLatin1());
        }
        return r;
    }

    FakeResponse putVersioning(const RecordedRequest& req, const QString& bucket)
    {
        if (std::optional<FakeResponse> bad = checkMd5(req))
            return *bad;
        QString status;
        QString mfa;
        QXmlStreamReader r(req.body);
        if (!r.readNextStartElement() || r.name() != QLatin1String("VersioningConfiguration"))
            return error(400, "MalformedXML", "The XML you provided was not well-formed");
        while (r.readNextStartElement()) {
            if (r.name() == QLatin1String("Status"))
                status = r.readElementText();
            else if (r.name() == QLatin1String("MfaDelete"))
                mfa = r.readElementText();
            else
                r.skipCurrentElement();
        }
        if (r.hasError() || (status != QLatin1String("Enabled") && status != QLatin1String("Suspended")))
            return error(400, "MalformedXML", "The XML you provided was not well-formed");
        if (!mfa.isEmpty() && req.header("x-amz-mfa").isNull())
            return error(400, "InvalidRequest", "MFA Delete can only be changed with MFA authentication");
        buckets[bucket].versioning = status;
        return FakeResponse::ok();
    }

    FakeResponse listVersions(const RecordedRequest& req, const QString& bucket)
    {
        const QString prefix = decode(req.queryItem("prefix"));
        const QString delimiter = decode(req.queryItem("delimiter"));
        const QByteArray keyMarker = decode(req.queryItem("key-marker")).toUtf8();
        const QString versionIdMarker = decode(req.queryItem("version-id-marker"));
        const int maxKeys = req.hasQueryItem("max-keys") ? std::clamp(req.queryItem("max-keys").toInt(), 1, 1000) : 1000;
        const bool encode = req.queryItem("encoding-type") == "url";
        const auto enc = [encode](const QString& text) {
            return encode ? uriEncodeText(text, SlashMode::Keep) : xmlEscape(text);
        };
        const QByteArray p = prefix.toUtf8();
        const QByteArray delim = delimiter.toUtf8();
        const auto& keys = buckets[bucket].keys;

        QByteArray body = "<ListVersionsResult><Name>" + xmlEscape(bucket) + "</Name><Prefix>" + enc(prefix)
                        + "</Prefix><KeyMarker>" + enc(QString::fromUtf8(keyMarker)) + "</KeyMarker><VersionIdMarker>"
                        + xmlEscape(versionIdMarker) + "</VersionIdMarker><MaxKeys>" + QByteArray::number(maxKeys)
                        + "</MaxKeys>";
        if (!delimiter.isEmpty())
            body += "<Delimiter>" + enc(delimiter) + "</Delimiter>";
        if (encode)
            body += "<EncodingType>url</EncodingType>";
        QByteArray entries;
        QByteArray prefixes;
        int returned = 0;
        bool truncated = false;
        QByteArray nextKey;
        QString nextVersionId;

        auto it = keys.lower_bound(keyMarker.isEmpty() ? p : keyMarker);
        // Continuing after a common prefix: nothing under it again.
        if (!delim.isEmpty() && !keyMarker.isEmpty() && keyMarker.endsWith(delim)) {
            while (it != keys.end() && it->first.startsWith(keyMarker))
                ++it;
        }
        while (it != keys.end() && !truncated) {
            const QByteArray& k = it->first;
            if (!k.startsWith(p)) {
                if (k > p)
                    break;
                ++it;
                continue;
            }
            std::size_t first = 0;
            if (!keyMarker.isEmpty() && k == keyMarker) {
                if (versionIdMarker.isEmpty()) {   // the marker key itself is done
                    ++it;
                    continue;
                }
                const auto& h = it->second;
                while (first < h.size() && h[first].versionId != versionIdMarker)
                    ++first;
                ++first;   // after the marker version
            }
            QByteArray common;
            if (!delim.isEmpty()) {
                const qsizetype at = k.indexOf(delim, p.size());
                if (at >= 0)
                    common = k.left(at + delim.size());
            }
            if (!common.isEmpty()) {
                if (returned == maxKeys) {
                    truncated = true;
                    break;
                }
                prefixes += "<CommonPrefixes><Prefix>" + enc(QString::fromUtf8(common)) + "</Prefix></CommonPrefixes>";
                nextKey = common;
                nextVersionId.clear();
                ++returned;
                while (it != keys.end() && it->first.startsWith(common))
                    ++it;
                continue;
            }
            const auto& h = it->second;
            for (std::size_t i = first; i < h.size(); ++i) {
                if (returned == maxKeys) {
                    truncated = true;
                    break;
                }
                const Version& v = h[i];
                const QByteArray fields = "<Key>" + enc(QString::fromUtf8(k)) + "</Key><VersionId>"
                                        + xmlEscape(v.versionId) + "</VersionId><IsLatest>"
                                        + QByteArray(i == 0 ? "true" : "false") + "</IsLatest><LastModified>"
                                        + iso(v.object.lastModified) + "</LastModified>";
                if (v.deleteMarker) {
                    entries += "<DeleteMarker>" + fields + "</DeleteMarker>";
                } else {
                    entries += "<Version>" + fields + "<ETag>" + xmlEscape(v.object.etag) + "</ETag><Size>"
                             + QByteArray::number(v.object.size) + "</Size><StorageClass>"
                             + (v.object.storageClass.isEmpty() ? QByteArray("STANDARD") : v.object.storageClass.toLatin1())
                             + "</StorageClass></Version>";
                }
                nextKey = k;
                nextVersionId = v.versionId;
                ++returned;
            }
            ++it;
        }
        body += "<IsTruncated>" + QByteArray(truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated) {
            body += "<NextKeyMarker>" + enc(QString::fromUtf8(nextKey)) + "</NextKeyMarker><NextVersionIdMarker>"
                  + xmlEscape(nextVersionId) + "</NextVersionIdMarker>";
        }
        return xmlResponse(body + entries + prefixes + "</ListVersionsResult>");
    }
};

FakeObjectStore::FakeObjectStore(FakeS3Server* server)
    : d(std::make_unique<Impl>())
{
    d->server = server;
    server->setFallbackHandler([impl = d.get()](const RecordedRequest& req) { return impl->handle(req); });
}

FakeObjectStore::~FakeObjectStore()
{
    if (d->server)
        d->server->setFallbackHandler({});
}

void FakeObjectStore::createBucket(const QString& bucket)
{
    d->buckets[bucket];
}

QString FakeObjectStore::putObject(const QString& bucket, const QString& key, const QByteArray& data,
                                   const QString& contentType)
{
    Object o;
    o.data = data;
    o.size = data.size();
    o.md5 = md5(data);
    o.etag = quoted(o.md5);
    o.lastModified = QDateTime::currentDateTimeUtc();
    o.contentType = contentType;
    return d->store(bucket, key, o);
}

QString FakeObjectStore::putSyntheticObject(const QString& bucket, const QString& key, qint64 size)
{
    Object o;
    o.size = size;
    o.etag = quoted(md5(key.toUtf8() + QByteArray::number(size)));
    o.lastModified = QDateTime::currentDateTimeUtc();
    o.contentType = QStringLiteral("application/octet-stream");
    return d->store(bucket, key, o);
}

void FakeObjectStore::setVersioning(const QString& bucket, const QString& status)
{
    d->buckets[bucket].versioning = status;
}

void FakeObjectStore::setMfaDelete(const QString& bucket, bool enabled)
{
    d->buckets[bucket].mfaDelete = enabled;
}

QString FakeObjectStore::deleteObject(const QString& bucket, const QString& key)
{
    return d->deleteCurrent(bucket, key);
}

void FakeObjectStore::lockVersion(const QString& bucket, const QString& key, const QString& versionId)
{
    if (Version* v = d->findVersion(bucket, key, versionId))
        v->locked = true;
}

void FakeObjectStore::setObjectLockEnabled(const QString& bucket)
{
    Impl::BucketData& b = d->buckets[bucket];
    b.objectLock = true;
    b.versioning = QStringLiteral("Enabled");
}

void FakeObjectStore::setBucketConfig(const QString& bucket, BucketConfig kind, const QByteArray& document)
{
    Impl::BucketData& b = d->buckets[bucket];
    if (kind == BucketConfig::Acl)
        b.acl = QString::fromLatin1(document);
    else if (kind == BucketConfig::ObjectLock)
        b.lockConfig = document;
    else if (document.isNull())
        b.configs.erase(bucketConfigSubresource(kind));
    else
        b.configs[bucketConfigSubresource(kind)] = document;
}

std::optional<QByteArray> FakeObjectStore::bucketConfig(const QString& bucket, BucketConfig kind) const
{
    const auto b = d->buckets.find(bucket);
    if (b == d->buckets.end())
        return std::nullopt;
    if (kind == BucketConfig::Acl)
        return b->second.acl.toLatin1();
    if (kind == BucketConfig::ObjectLock)
        return b->second.lockConfig.isEmpty() ? std::nullopt : std::optional<QByteArray>(b->second.lockConfig);
    const auto it = b->second.configs.find(bucketConfigSubresource(kind));
    return it == b->second.configs.end() ? std::nullopt : std::optional<QByteArray>(it->second);
}

void FakeObjectStore::setUnsupportedBucketConfig(BucketConfig kind)
{
    d->unsupportedConfigs.append(kind);
}

void FakeObjectStore::setRetention(const QString& bucket, const QString& key, const QString& versionId,
                                   ObjectLockMode mode, const QDateTime& until)
{
    if (Version* v = d->findVersion(bucket, key, versionId)) {
        v->object.headers.insert(QLatin1String(kLockMode), objectLockModeName(mode));
        v->object.headers.insert(QLatin1String(kLockUntil), QString::fromLatin1(iso(until)));
    }
}

void FakeObjectStore::setLegalHold(const QString& bucket, const QString& key, const QString& versionId, bool on)
{
    if (Version* v = d->findVersion(bucket, key, versionId)) {
        if (on)
            v->object.headers.insert(QLatin1String(kLegalHold), QStringLiteral("ON"));
        else
            v->object.headers.remove(QLatin1String(kLegalHold));
    }
}

void FakeObjectStore::setInterceptSubresourceWrites(bool intercept)
{
    d->interceptSubresourceWrites = intercept;
}

bool FakeObjectStore::hasBucket(const QString& bucket) const
{
    return d->buckets.find(bucket) != d->buckets.end();
}

void FakeObjectStore::setDiscardData(bool discard)
{
    d->discard = discard;
}

void FakeObjectStore::setMinPartSize(qint64 bytes)
{
    d->minPartSize = bytes;
}

void FakeObjectStore::setHonourIfNoneMatch(bool honour)
{
    d->honourIfNoneMatch = honour;
}

void FakeObjectStore::setHonourIfMatch(bool honour)
{
    d->honourIfMatch = honour;
}

void FakeObjectStore::setIgnoreRanges(bool ignore)
{
    d->ignoreRanges = ignore;
}

void FakeObjectStore::setIgnoreMetadataReplace(bool ignore)
{
    d->ignoreMetadataReplace = ignore;
}

void FakeObjectStore::setDropObjectSubresources(bool drop)
{
    d->dropObjectSubresources = drop;
}

void FakeObjectStore::setDropTagsOnReplace(bool drop)
{
    d->dropTagsOnReplace = drop;
}

void FakeObjectStore::setObjectHeader(const QString& bucket, const QString& key, const QString& name,
                                      const QString& value)
{
    if (Object* o = d->find(bucket, key))
        o->headers.insert(name.toLower(), value);
}

void FakeObjectStore::setObjectAcl(const QString& bucket, const QString& key, const QString& cannedAcl)
{
    if (Object* o = d->find(bucket, key))
        o->acl = cannedAcl;
}

void FakeObjectStore::setObjectTags(const QString& bucket, const QString& key, const TagSet& tags)
{
    if (Object* o = d->find(bucket, key))
        o->tags = tags;
}

void FakeObjectStore::setObjectStorageClass(const QString& bucket, const QString& key, const QString& storageClass)
{
    if (Object* o = d->find(bucket, key))
        o->storageClass = storageClass;
}

void FakeObjectStore::addFault(const Fault& fault)
{
    d->faults.append(fault);
    d->faultHits.append(0);
}

void FakeObjectStore::setDelay(const QString& operation, int ms)
{
    d->delays.insert(operation, ms);
}

void FakeObjectStore::clearFaults()
{
    d->faults.clear();
    d->faultHits.clear();
}

std::optional<FakeObjectStore::Object> FakeObjectStore::object(const QString& bucket, const QString& key) const
{
    if (const Object* o = d->find(bucket, key))
        return *o;
    return std::nullopt;
}

QStringList FakeObjectStore::keys(const QString& bucket) const
{
    QStringList out;
    const auto b = d->buckets.find(bucket);
    if (b == d->buckets.end())
        return out;
    for (const auto& [key, h] : b->second.keys) {
        if (!h.empty() && !h.front().deleteMarker)
            out.append(QString::fromUtf8(key));
    }
    return out;
}

QList<FakeObjectStore::Version> FakeObjectStore::versions(const QString& bucket, const QString& key) const
{
    QList<Version> out;
    if (const std::vector<Version>* h = d->history(bucket, key))
        out.assign(h->begin(), h->end());
    return out;
}

QStringList FakeObjectStore::versionedKeys(const QString& bucket) const
{
    QStringList out;
    const auto b = d->buckets.find(bucket);
    if (b == d->buckets.end())
        return out;
    for (const auto& [key, h] : b->second.keys) {
        if (!h.empty())
            out.append(QString::fromUtf8(key));
    }
    return out;
}

QString FakeObjectStore::versioning(const QString& bucket) const
{
    const auto b = d->buckets.find(bucket);
    return b == d->buckets.end() ? QString() : b->second.versioning;
}

QStringList FakeObjectStore::uploadIds(const QString& bucket) const
{
    QStringList out;
    for (const auto& [id, u] : d->uploads) {
        if (u.bucket == bucket)
            out.append(id);
    }
    return out;
}

QMap<int, qint64> FakeObjectStore::uploadParts(const QString& uploadId) const
{
    QMap<int, qint64> out;
    const auto it = d->uploads.find(uploadId);
    if (it == d->uploads.end())
        return out;
    for (const auto& [n, p] : it->second.parts)
        out.insert(n, p.size);
    return out;
}

QList<QPair<QString, int>> FakeObjectStore::partUploads() const
{
    return d->partUploads;
}

int FakeObjectStore::count(const QString& operation) const
{
    return d->counts.value(operation);
}

QString FakeObjectStore::compositeEtag(const QList<QByteArray>& partMd5s)
{
    QByteArray all;
    for (const QByteArray& m : partMd5s)
        all += m;
    return QLatin1Char('"') + QString::fromLatin1(md5(all).toHex()) + QLatin1Char('-')
         + QString::number(partMd5s.size()) + QLatin1Char('"');
}

} // namespace qbucket::s3::testing
