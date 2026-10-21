#include "xml/XmlWriters.h"

namespace qbucket::s3::xml {
namespace {

bool representableInXml(const QString& text)
{
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x20 && u != 0x09 && u != 0x0A && u != 0x0D)
            return false;
        if (u == 0xFFFE || u == 0xFFFF)
            return false;
    }
    return true;
}

} // namespace

QByteArray escapeText(const QString& text)
{
    QByteArray out;
    const QByteArray utf8 = text.toUtf8();
    out.reserve(utf8.size() + 16);
    for (const char c : utf8) {
        switch (c) {
        case '&':  out += "&amp;"; break;
        case '<':  out += "&lt;"; break;
        case '>':  out += "&gt;"; break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        case '\r': out += "&#13;"; break;
        case '\n': out += "&#10;"; break;
        case '\t': out += "&#9;"; break;
        default:   out += c; break;
        }
    }
    return out;
}

Result<QByteArray> buildDeleteRequest(const QList<ObjectIdentifier>& objects, bool quiet)
{
    QByteArray out = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                     R"(<Delete xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)";
    if (quiet)
        out += "<Quiet>true</Quiet>";
    for (const ObjectIdentifier& o : objects) {
        if (!representableInXml(o.key)) {
            return Error::make(ErrorCategory::InvalidRequest,
                               QStringLiteral("key contains characters that cannot be sent in a "
                                              "batch delete; delete it individually"));
        }
        out += "<Object><Key>" + escapeText(o.key) + "</Key>";
        if (!o.versionId.isEmpty())
            out += "<VersionId>" + escapeText(o.versionId) + "</VersionId>";
        out += "</Object>";
    }
    out += "</Delete>";
    return out;
}

QByteArray buildCompleteMultipartUpload(const QList<CompletedPart>& parts)
{
    QByteArray out = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                     R"(<CompleteMultipartUpload xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)";
    for (const CompletedPart& p : parts) {
        out += "<Part><PartNumber>" + QByteArray::number(p.partNumber) + "</PartNumber>"
             + "<ETag>" + escapeText(p.etag) + "</ETag></Part>";
    }
    out += "</CompleteMultipartUpload>";
    return out;
}

Result<QByteArray> buildTagging(const TagSet& tags)
{
    QByteArray out = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                     R"(<Tagging xmlns="http://s3.amazonaws.com/doc/2006-03-01/"><TagSet>)";
    for (const Tag& t : tags) {
        if (!representableInXml(t.key) || !representableInXml(t.value)) {
            return Error::make(ErrorCategory::InvalidRequest,
                               QStringLiteral("tag contains characters that cannot be sent: ") + t.key);
        }
        out += "<Tag><Key>" + escapeText(t.key) + "</Key><Value>" + escapeText(t.value) + "</Value></Tag>";
    }
    out += "</TagSet></Tagging>";
    return out;
}

QByteArray buildCreateBucketConfiguration(const QString& locationConstraint)
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>)"
           R"(<CreateBucketConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)"
           "<LocationConstraint>" + escapeText(locationConstraint) + "</LocationConstraint>"
           "</CreateBucketConfiguration>";
}

} // namespace qbucket::s3::xml
