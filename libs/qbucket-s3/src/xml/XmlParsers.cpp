#include "xml/XmlParsers.h"

#include "http/UriEncoding.h"

#include <QXmlStreamReader>

namespace qbucket::s3::xml {
namespace {

Error protocolError(const QString& message)
{
    return Error::make(ErrorCategory::Protocol, message);
}

// Positions the reader on the root element. Fails for non-XML bodies and
// for an unexpected root.
std::optional<Error> openRoot(QXmlStreamReader& r, QLatin1StringView expectedRoot)
{
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (r.name() != expectedRoot) {
                return protocolError(QStringLiteral("unexpected XML root element <%1>, expected <%2>")
                                         .arg(r.name().toString(), QString(expectedRoot)));
            }
            return std::nullopt;
        }
    }
    return protocolError(r.hasError()
                             ? QStringLiteral("malformed XML: %1").arg(r.errorString())
                             : QStringLiteral("empty XML document, expected <%1>").arg(QString(expectedRoot)));
}

std::optional<Error> finish(QXmlStreamReader& r)
{
    // Drain to detect truncation after the last element we cared about.
    while (!r.atEnd())
        r.readNext();
    if (r.hasError())
        return protocolError(QStringLiteral("malformed XML: %1").arg(r.errorString()));
    return std::nullopt;
}

bool parseBool(const QString& text)
{
    return text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

// Decodes an encoding-type=url value; plain copy otherwise.
bool decodeValue(const QString& raw, bool encoded, bool plusIsSpace, QString* out)
{
    if (!encoded) {
        *out = raw;
        return true;
    }
    QByteArray bytes;
    if (!uriDecode(QByteArrayView(raw.toUtf8()), plusIsSpace, &bytes))
        return false;
    *out = QString::fromUtf8(bytes);
    return true;
}

// Shared by V1 and V2: everything inside <ListBucketResult>.
struct RawListing {
    QString name;
    QString prefix;
    QString delimiter;
    QString encodingType;
    QString nextContinuationToken;
    QString nextMarker;
    bool isTruncated = false;
    QList<ObjectEntry> objects;   // keys still raw
    QStringList commonPrefixes;   // raw
};

Result<RawListing> readListing(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("ListBucketResult")))
        return *err;

    RawListing out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Contents")) {
            ObjectEntry e;
            while (r.readNextStartElement()) {
                const auto c = r.name();
                if (c == QLatin1String("Key"))
                    e.key = r.readElementText();
                else if (c == QLatin1String("Size"))
                    e.size = r.readElementText().toLongLong();
                else if (c == QLatin1String("LastModified"))
                    e.lastModified = parseIsoTimestamp(r.readElementText());
                else if (c == QLatin1String("ETag"))
                    e.etag = r.readElementText();
                else if (c == QLatin1String("StorageClass"))
                    e.storageClass = r.readElementText();
                else
                    r.skipCurrentElement();
            }
            out.objects.append(e);
        } else if (n == QLatin1String("CommonPrefixes")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("Prefix"))
                    out.commonPrefixes.append(r.readElementText());
                else
                    r.skipCurrentElement();
            }
        } else if (n == QLatin1String("Name")) {
            out.name = r.readElementText();
        } else if (n == QLatin1String("Prefix")) {
            out.prefix = r.readElementText();
        } else if (n == QLatin1String("Delimiter")) {
            out.delimiter = r.readElementText();
        } else if (n == QLatin1String("EncodingType")) {
            out.encodingType = r.readElementText();
        } else if (n == QLatin1String("IsTruncated")) {
            out.isTruncated = parseBool(r.readElementText());
        } else if (n == QLatin1String("NextContinuationToken")) {
            out.nextContinuationToken = r.readElementText();
        } else if (n == QLatin1String("NextMarker")) {
            out.nextMarker = r.readElementText();
        } else {
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    return out;
}

Result<ListPage> toPage(RawListing&& raw, bool plusIsSpace)
{
    const bool encoded = raw.encodingType.compare(QLatin1String("url"), Qt::CaseInsensitive) == 0;
    const auto decodeError = [](const QString& what) {
        return protocolError(QStringLiteral("invalid url-encoded %1 in listing").arg(what));
    };

    ListPage page;
    page.bucket = raw.name;
    page.isTruncated = raw.isTruncated;
    page.nextContinuationToken = raw.nextContinuationToken;
    if (!decodeValue(raw.prefix, encoded, plusIsSpace, &page.prefix))
        return decodeError(QStringLiteral("prefix"));
    if (!decodeValue(raw.delimiter, encoded, plusIsSpace, &page.delimiter))
        return decodeError(QStringLiteral("delimiter"));

    page.objects = std::move(raw.objects);
    for (ObjectEntry& e : page.objects) {
        if (!decodeValue(e.key, encoded, plusIsSpace, &e.key))
            return decodeError(QStringLiteral("key"));
    }
    page.commonPrefixes.reserve(raw.commonPrefixes.size());
    for (const QString& p : std::as_const(raw.commonPrefixes)) {
        QString decoded;
        if (!decodeValue(p, encoded, plusIsSpace, &decoded))
            return decodeError(QStringLiteral("common prefix"));
        page.commonPrefixes.append(decoded);
    }
    return page;
}

} // namespace

QDateTime parseIsoTimestamp(const QString& text)
{
    QDateTime dt = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(text, Qt::ISODate);
    return dt.isValid() ? dt.toUTC() : QDateTime();
}

QString rootElementName(const QByteArray& body)
{
    QXmlStreamReader r(body);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement())
            return r.name().toString();
    }
    return {};
}

std::optional<ErrorBody> parseErrorBody(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (openRoot(r, QLatin1StringView("Error")))
        return std::nullopt;
    ErrorBody e;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Code"))
            e.code = r.readElementText();
        else if (n == QLatin1String("Message"))
            e.message = r.readElementText();
        else if (n == QLatin1String("Resource"))
            e.resource = r.readElementText();
        else if (n == QLatin1String("RequestId"))
            e.requestId = r.readElementText();
        else if (n == QLatin1String("HostId"))
            e.hostId = r.readElementText();
        else if (n == QLatin1String("Region"))
            e.region = r.readElementText();
        else if (n == QLatin1String("Endpoint"))
            e.endpoint = r.readElementText();
        else if (n == QLatin1String("ServerTime"))
            e.serverTime = parseIsoTimestamp(r.readElementText());
        else
            r.skipCurrentElement();
    }
    // A truncated error body still tells us the code; don't insist on finish().
    if (e.code.isEmpty())
        return std::nullopt;
    return e;
}

Result<BucketList> parseListBuckets(const QByteArray& body, QString* continuationToken)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("ListAllMyBucketsResult")))
        return *err;

    BucketList out;
    QString token;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Owner")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("ID"))
                    out.ownerId = r.readElementText();
                else if (r.name() == QLatin1String("DisplayName"))
                    out.ownerDisplayName = r.readElementText();
                else
                    r.skipCurrentElement();
            }
        } else if (n == QLatin1String("Buckets")) {
            while (r.readNextStartElement()) {
                if (r.name() != QLatin1String("Bucket")) {
                    r.skipCurrentElement();
                    continue;
                }
                BucketInfo b;
                while (r.readNextStartElement()) {
                    const auto c = r.name();
                    if (c == QLatin1String("Name"))
                        b.name = r.readElementText();
                    else if (c == QLatin1String("CreationDate"))
                        b.creationDate = parseIsoTimestamp(r.readElementText());
                    else if (c == QLatin1String("BucketRegion"))
                        b.region = r.readElementText();
                    else
                        r.skipCurrentElement();
                }
                out.buckets.append(b);
            }
        } else if (n == QLatin1String("ContinuationToken")) {
            token = r.readElementText();
        } else {
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    if (continuationToken)
        *continuationToken = token;
    return out;
}

Result<ListPage> parseListObjectsV2(const QByteArray& body, bool plusIsSpace)
{
    Result<RawListing> raw = readListing(body);
    if (!raw)
        return raw.error();
    if (raw->isTruncated && raw->nextContinuationToken.isEmpty())
        return protocolError(QStringLiteral("truncated listing without NextContinuationToken"));
    return toPage(std::move(raw).value(), plusIsSpace);
}

Result<ListPage> parseListObjectsV1(const QByteArray& body, bool plusIsSpace)
{
    Result<RawListing> raw = readListing(body);
    if (!raw)
        return raw.error();
    const bool encoded = raw->encodingType.compare(QLatin1String("url"), Qt::CaseInsensitive) == 0;
    const QString nextMarker = raw->nextMarker;
    Result<ListPage> page = toPage(std::move(raw).value(), plusIsSpace);
    if (!page || !page->isTruncated)
        return page;

    // V1: NextMarker (encoded like keys), else the greatest key/prefix seen.
    QString marker;
    if (!decodeValue(nextMarker, encoded, plusIsSpace, &marker))
        return protocolError(QStringLiteral("invalid url-encoded NextMarker in listing"));
    if (marker.isEmpty()) {
        if (!page->objects.isEmpty())
            marker = page->objects.last().key;
        if (!page->commonPrefixes.isEmpty() && page->commonPrefixes.last() > marker)
            marker = page->commonPrefixes.last();
    }
    if (marker.isEmpty())
        return protocolError(QStringLiteral("truncated V1 listing without a marker"));
    page->nextContinuationToken = marker;
    return page;
}

Result<CreateMultipartUploadResult> parseInitiateMultipartUpload(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("InitiateMultipartUploadResult")))
        return *err;
    CreateMultipartUploadResult out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Bucket"))
            out.bucket = r.readElementText();
        else if (n == QLatin1String("Key"))
            out.key = r.readElementText();
        else if (n == QLatin1String("UploadId"))
            out.uploadId = r.readElementText();
        else
            r.skipCurrentElement();
    }
    if (auto err = finish(r))
        return *err;
    if (out.uploadId.isEmpty())
        return protocolError(QStringLiteral("InitiateMultipartUploadResult without an UploadId"));
    return out;
}

Result<CompleteMultipartUploadResult> parseCompleteMultipartUpload(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("CompleteMultipartUploadResult")))
        return *err;
    CompleteMultipartUploadResult out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("ETag"))
            out.etag = r.readElementText();
        else if (n == QLatin1String("Location"))
            out.location = r.readElementText();
        else
            r.skipCurrentElement();
    }
    if (auto err = finish(r))
        return *err;
    if (out.etag.isEmpty())
        return protocolError(QStringLiteral("CompleteMultipartUploadResult without an ETag"));
    return out;
}

Result<ListPartsPage> parseListParts(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("ListPartsResult")))
        return *err;
    ListPartsPage out;
    bool haveNext = false;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Part")) {
            PartInfo p;
            while (r.readNextStartElement()) {
                const auto c = r.name();
                if (c == QLatin1String("PartNumber"))
                    p.partNumber = r.readElementText().toInt();
                else if (c == QLatin1String("ETag"))
                    p.etag = r.readElementText();
                else if (c == QLatin1String("Size"))
                    p.size = r.readElementText().toLongLong();
                else if (c == QLatin1String("LastModified"))
                    p.lastModified = parseIsoTimestamp(r.readElementText());
                else
                    r.skipCurrentElement();
            }
            if (p.partNumber <= 0)
                return protocolError(QStringLiteral("ListPartsResult with an invalid part number"));
            out.parts.append(p);
        } else if (n == QLatin1String("IsTruncated")) {
            out.isTruncated = parseBool(r.readElementText());
        } else if (n == QLatin1String("NextPartNumberMarker")) {
            bool ok = false;
            out.nextPartNumberMarker = r.readElementText().toInt(&ok);
            haveNext = ok;
        } else if (n == QLatin1String("StorageClass")) {
            out.storageClass = r.readElementText();
        } else {
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    if (out.isTruncated && !haveNext) {
        if (out.parts.isEmpty())
            return protocolError(QStringLiteral("truncated ListPartsResult without NextPartNumberMarker"));
        out.nextPartNumberMarker = out.parts.last().partNumber;
    }
    return out;
}

Result<ListMultipartUploadsPage> parseListMultipartUploads(const QByteArray& body, bool plusIsSpace)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("ListMultipartUploadsResult")))
        return *err;
    ListMultipartUploadsPage out;
    QString encodingType;
    QString nextKeyMarker;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Upload")) {
            MultipartUploadInfo u;
            while (r.readNextStartElement()) {
                const auto c = r.name();
                if (c == QLatin1String("Key")) {
                    u.key = r.readElementText();
                } else if (c == QLatin1String("UploadId")) {
                    u.uploadId = r.readElementText();
                } else if (c == QLatin1String("Initiated")) {
                    u.initiated = parseIsoTimestamp(r.readElementText());
                } else if (c == QLatin1String("StorageClass")) {
                    u.storageClass = r.readElementText();
                } else if (c == QLatin1String("Initiator")) {
                    QString id;
                    QString name;
                    while (r.readNextStartElement()) {
                        if (r.name() == QLatin1String("DisplayName"))
                            name = r.readElementText();
                        else if (r.name() == QLatin1String("ID"))
                            id = r.readElementText();
                        else
                            r.skipCurrentElement();
                    }
                    u.initiator = name.isEmpty() ? id : name;
                } else {
                    r.skipCurrentElement();
                }
            }
            out.uploads.append(u);
        } else if (n == QLatin1String("IsTruncated")) {
            out.isTruncated = parseBool(r.readElementText());
        } else if (n == QLatin1String("NextKeyMarker")) {
            nextKeyMarker = r.readElementText();
        } else if (n == QLatin1String("NextUploadIdMarker")) {
            out.nextUploadIdMarker = r.readElementText();
        } else if (n == QLatin1String("EncodingType")) {
            encodingType = r.readElementText();
        } else {
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    const bool encoded = encodingType.compare(QLatin1String("url"), Qt::CaseInsensitive) == 0;
    for (MultipartUploadInfo& u : out.uploads) {
        if (!decodeValue(u.key, encoded, plusIsSpace, &u.key))
            return protocolError(QStringLiteral("invalid url-encoded key in ListMultipartUploadsResult"));
    }
    if (!decodeValue(nextKeyMarker, encoded, plusIsSpace, &out.nextKeyMarker))
        return protocolError(QStringLiteral("invalid url-encoded NextKeyMarker"));
    if (out.isTruncated && out.nextKeyMarker.isEmpty()) {
        if (out.uploads.isEmpty())
            return protocolError(QStringLiteral("truncated ListMultipartUploadsResult without NextKeyMarker"));
        out.nextKeyMarker = out.uploads.last().key;
        out.nextUploadIdMarker = out.uploads.last().uploadId;
    }
    return out;
}

Result<UploadPartCopyResult> parseCopyPartResult(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("CopyPartResult")))
        return *err;
    UploadPartCopyResult out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("ETag"))
            out.etag = r.readElementText();
        else if (n == QLatin1String("LastModified"))
            out.lastModified = parseIsoTimestamp(r.readElementText());
        else
            r.skipCurrentElement();
    }
    if (auto err = finish(r))
        return *err;
    if (out.etag.isEmpty())
        return protocolError(QStringLiteral("CopyPartResult without an ETag"));
    return out;
}

Result<CopyObjectResult> parseCopyObjectResult(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("CopyObjectResult")))
        return *err;
    CopyObjectResult out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("ETag"))
            out.etag = r.readElementText();
        else if (n == QLatin1String("LastModified"))
            out.lastModified = parseIsoTimestamp(r.readElementText());
        else
            r.skipCurrentElement();
    }
    if (r.hasError())
        return Error::make(ErrorCategory::Protocol, QStringLiteral("malformed CopyObjectResult: ") + r.errorString());
    if (out.etag.isEmpty())
        return Error::make(ErrorCategory::Protocol, QStringLiteral("CopyObjectResult without an ETag"));
    return out;
}

Result<DeleteObjectsResult> parseDeleteResult(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("DeleteResult")))
        return *err;

    DeleteObjectsResult out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Deleted")) {
            DeleteObjectsResult::Deleted d;
            while (r.readNextStartElement()) {
                const auto c = r.name();
                if (c == QLatin1String("Key"))
                    d.key = r.readElementText();
                else if (c == QLatin1String("VersionId"))
                    d.versionId = r.readElementText();
                else if (c == QLatin1String("DeleteMarker"))
                    d.deleteMarker = parseBool(r.readElementText());
                else if (c == QLatin1String("DeleteMarkerVersionId"))
                    d.deleteMarkerVersionId = r.readElementText();
                else
                    r.skipCurrentElement();
            }
            out.deleted.append(d);
        } else if (n == QLatin1String("Error")) {
            DeleteObjectsResult::Failed f;
            while (r.readNextStartElement()) {
                const auto c = r.name();
                if (c == QLatin1String("Key"))
                    f.key = r.readElementText();
                else if (c == QLatin1String("VersionId"))
                    f.versionId = r.readElementText();
                else if (c == QLatin1String("Code"))
                    f.code = r.readElementText();
                else if (c == QLatin1String("Message"))
                    f.message = r.readElementText();
                else
                    r.skipCurrentElement();
            }
            out.failed.append(f);
        } else {
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    return out;
}

Result<QString> parseLocationConstraint(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("LocationConstraint")))
        return *err;
    QString value = r.readElementText().trimmed();
    if (auto err = finish(r))
        return *err;
    return value;
}

} // namespace qbucket::s3::xml
