#include <qbucket/s3/testing/FakeObjectStore.h>

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

QString cannedAcl(const RecordedRequest& req)
{
    const QByteArray acl = req.header("x-amz-acl");
    return acl.isNull() ? QStringLiteral("private") : QString::fromLatin1(acl);
}

} // namespace

struct FakeObjectStore::Impl {
    QPointer<FakeS3Server> server;
    std::map<QString, std::map<QByteArray, Object>> buckets;   // keys in UTF-8 byte order
    std::map<QString, Upload> uploads;                          // by id (sortable)
    QList<QPair<QString, int>> partUploads;
    QHash<QString, int> counts;
    QList<Fault> faults;
    QList<int> faultHits;   // matches per fault
    QHash<QString, int> delays;
    int nextUpload = 1;
    bool discard = false;
    qint64 minPartSize = Q_INT64_C(5) * 1024 * 1024;
    bool honourIfNoneMatch = true;
    bool ignoreRanges = false;
    bool ignoreMetadataReplace = false;
    bool dropObjectSubresources = false;
    bool dropTagsOnReplace = false;

    Object* find(const QString& bucket, const QString& key)
    {
        auto b = buckets.find(bucket);
        if (b == buckets.end())
            return nullptr;
        auto o = b->second.find(key.toUtf8());
        return o == b->second.end() ? nullptr : &o->second;
    }

    QString operationOf(const RecordedRequest& req, const QString& key) const
    {
        const QByteArray& m = req.method;
        if (req.path == "/")
            return QStringLiteral("ListBuckets");
        if (key.isEmpty()) {
            if (m == "GET" && req.hasQueryItem("uploads"))
                return QStringLiteral("ListMultipartUploads");
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
        if (m == "POST" && req.hasQueryItem("uploads"))
            return QStringLiteral("CreateMultipartUpload");
        if (m == "POST" && req.hasQueryItem("uploadId"))
            return QStringLiteral("CompleteMultipartUpload");
        if (m == "PUT" && req.hasQueryItem("uploadId"))
            return req.header("x-amz-copy-source").isNull() ? QStringLiteral("UploadPart") : QStringLiteral("UploadPartCopy");
        // Never let an unknown subresource fall through to an upload.
        const bool plain = req.query.isEmpty() || (dropObjectSubresources && (req.hasQueryItem("tagging") || req.hasQueryItem("acl")));
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
        if (buckets.find(bucket) == buckets.end())
            return error(404, "NoSuchBucket", "The specified bucket does not exist");
        if (op == QLatin1String("HeadBucket"))
            return FakeResponse::ok();
        if (op == QLatin1String("ListObjectsV2"))
            return listObjects(req, bucket);
        if (op == QLatin1String("ListMultipartUploads"))
            return listUploads(req, bucket);
        if (op == QLatin1String("DeleteObjects"))
            return deleteObjects(req, bucket);
        if (op == QLatin1String("GetBucketSubresource")) {
            if (req.hasQueryItem("versioning"))
                return xmlResponse("<VersioningConfiguration/>");
            return error(501, "NotImplemented", "not implemented by FakeObjectStore");
        }
        if (op == QLatin1String("PutObject"))
            return putObject(req, bucket, key);
        if (op == QLatin1String("CopyObject"))
            return copyObject(req, bucket, key);
        if (op == QLatin1String("GetObject") || op == QLatin1String("HeadObject"))
            return getObject(req, bucket, key, op == QLatin1String("HeadObject"));
        if (op == QLatin1String("DeleteObject")) {
            buckets[bucket].erase(key.toUtf8());
            FakeResponse r;
            r.status = 204;
            return r;
        }
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
        Object* o = find(bucket, key);
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
        if (op == QLatin1String("GetObjectAcl")) {
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
            if (o->acl == QLatin1String("public-read") || o->acl == QLatin1String("public-read-write"))
                group("AllUsers", "READ");
            if (o->acl == QLatin1String("public-read-write"))
                group("AllUsers", "WRITE");
            if (o->acl == QLatin1String("authenticated-read"))
                group("AuthenticatedUsers", "READ");
            return xmlResponse(body + "</AccessControlList></AccessControlPolicy>");
        }
        // PutObjectAcl (canned only)
        const QByteArray acl = req.header("x-amz-acl");
        if (acl.isEmpty())
            return error(400, "MissingSecurityHeader", "x-amz-acl is required by FakeObjectStore");
        o->acl = QString::fromLatin1(acl);
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

    FakeResponse stored(const Object& o)
    {
        return FakeResponse::ok().withHeader("ETag", o.etag.toLatin1());
    }

    FakeResponse putObject(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        if (std::optional<FakeResponse> bad = checkMd5(req))
            return *bad;
        if (blockedByIfNoneMatch(req, bucket, key))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
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
        buckets[bucket][key.toUtf8()] = o;
        return stored(o);
    }

    // x-amz-copy-source → (bucket, key), the source object.
    std::optional<std::pair<QString, Object*>> copySource(const RecordedRequest& req)
    {
        QByteArray raw = req.header("x-amz-copy-source");
        if (const qsizetype q = raw.indexOf('?'); q >= 0)
            raw = raw.left(q);
        if (raw.startsWith('/'))
            raw = raw.mid(1);
        const qsizetype slash = raw.indexOf('/');
        if (slash <= 0)
            return std::nullopt;
        const QString bucket = decode(raw.left(slash));
        const QString key = decode(raw.mid(slash + 1));
        Object* o = find(bucket, key);
        if (!o)
            return std::nullopt;
        return std::pair{bucket, o};
    }

    FakeResponse copyObject(const RecordedRequest& req, const QString& bucket, const QString& key)
    {
        const auto source = copySource(req);
        if (!source)
            return error(404, "NoSuchKey", "The specified key does not exist.");
        const QByteArray ifMatch = req.header("x-amz-copy-source-if-match");
        if (!ifMatch.isNull() && unquoted(QString::fromLatin1(ifMatch)) != unquoted(source->second->etag))
            return error(412, "PreconditionFailed", "At least one of the pre-conditions you specified did not hold");
        const bool replace = req.header("x-amz-metadata-directive").compare("REPLACE", Qt::CaseInsensitive) == 0;
        const QString storageClass = QString::fromLatin1(req.header("x-amz-storage-class"));
        // AWS refuses a copy onto itself that changes nothing.
        if (source->first == bucket && source->second == find(bucket, key) && !replace && storageClass.isEmpty()
            && req.header("x-amz-server-side-encryption").isNull()) {
            return error(400, "InvalidRequest",
                         "This copy request is illegal because it is trying to copy an object to itself without "
                         "changing the object's metadata, storage class, website redirect location or encryption "
                         "attributes.");
        }
        Object copy = *source->second;
        copy.lastModified = QDateTime::currentDateTimeUtc();
        copy.storageClass = storageClass;
        // The encryption comes from the request (or the bucket default), not the source.
        copy.headers.remove(QStringLiteral("x-amz-server-side-encryption"));
        if (replace && !ignoreMetadataReplace) {
            copy.contentType = QString::fromUtf8(req.header("content-type"));
            copy.metadata = userMetadata(req);
            copy.headers = storedHeaders(req);
            if (dropTagsOnReplace && source->second == find(bucket, key))
                copy.tags.clear();
        } else if (const QByteArray sse = req.header("x-amz-server-side-encryption"); !sse.isNull()) {
            copy.headers.insert(QStringLiteral("x-amz-server-side-encryption"), QString::fromLatin1(sse));
        }
        copy.acl = cannedAcl(req);   // never copied
        if (req.header("x-amz-tagging-directive").compare("REPLACE", Qt::CaseInsensitive) == 0)
            copy.tags = taggingHeader(req);
        buckets[bucket][key.toUtf8()] = copy;
        return xmlResponse("<CopyObjectResult><LastModified>" + iso(copy.lastModified) + "</LastModified><ETag>"
                           + xmlEscape(copy.etag) + "</ETag></CopyObjectResult>");
    }

    FakeResponse getObject(const RecordedRequest& req, const QString& bucket, const QString& key, bool head)
    {
        const Object* o = find(bucket, key);
        if (!o)
            return head ? error(404, "", "") : error(404, "NoSuchKey", "The specified key does not exist.");
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
            const auto source = copySource(req);
            if (!source)
                return error(404, "NoSuchKey", "The specified key does not exist.");
            const Object& src = *source->second;
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
        buckets[bucket][key.toUtf8()] = o;
        uploads.erase(u->id);
        return xmlResponse("<CompleteMultipartUploadResult><Location>http://fake/" + xmlEscape(bucket) + '/'
                           + xmlEscape(key) + "</Location><Bucket>" + xmlEscape(bucket) + "</Bucket><Key>"
                           + xmlEscape(key) + "</Key><ETag>" + xmlEscape(o.etag)
                           + "</ETag></CompleteMultipartUploadResult>");
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

        const auto& objects = buckets[bucket];
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
            const Object& o = it->second;
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
        QByteArray body = "<DeleteResult>";
        QXmlStreamReader r(req.body);
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("Key")) {
                const QString key = r.readElementText();
                buckets[bucket].erase(key.toUtf8());
                body += "<Deleted><Key>" + xmlEscape(key) + "</Key></Deleted>";
            }
        }
        if (r.hasError())
            return error(400, "MalformedXML", "The XML you provided was not well-formed");
        return xmlResponse(body + "</DeleteResult>");
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

void FakeObjectStore::putObject(const QString& bucket, const QString& key, const QByteArray& data,
                                const QString& contentType)
{
    Object o;
    o.data = data;
    o.size = data.size();
    o.md5 = md5(data);
    o.etag = quoted(o.md5);
    o.lastModified = QDateTime::currentDateTimeUtc();
    o.contentType = contentType;
    d->buckets[bucket][key.toUtf8()] = o;
}

void FakeObjectStore::putSyntheticObject(const QString& bucket, const QString& key, qint64 size)
{
    Object o;
    o.size = size;
    o.etag = quoted(md5(key.toUtf8() + QByteArray::number(size)));
    o.lastModified = QDateTime::currentDateTimeUtc();
    o.contentType = QStringLiteral("application/octet-stream");
    d->buckets[bucket][key.toUtf8()] = o;
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
    for (const auto& [key, o] : b->second)
        out.append(QString::fromUtf8(key));
    return out;
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
