#pragma once

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QList>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace qbucket::testing {

struct FakeObject {
    QString key;
    qint64 size = 0;
    QByteArray storageClass = "STANDARD";
};

// Answers ListObjectsV2 like S3 does — prefix, "/" delimiter with common
// prefixes, max-keys, continuation tokens — from a key list held in
// memory. Keys are compared as UTF-8 bytes (S3 order); XML-special
// characters are escaped, keys are sent without URL encoding.
class FakeBucket {
public:
    explicit FakeBucket(QList<FakeObject> objects = {})
        : m_objects(std::move(objects))
    {
        sort();
    }

    void add(const QString& key, qint64 size = 0, const QByteArray& storageClass = "STANDARD")
    {
        m_objects.append({key, size, storageClass});
    }
    void sort()
    {
        std::sort(m_objects.begin(), m_objects.end(),
                  [](const FakeObject& a, const FakeObject& b) { return a.key.toUtf8() < b.key.toUtf8(); });
    }
    [[nodiscard]] const QList<FakeObject>& objects() const { return m_objects; }
    [[nodiscard]] bool contains(const QString& key) const { return indexOf(key) >= 0; }
    [[nodiscard]] QStringList keys() const
    {
        QStringList out;
        for (const FakeObject& o : m_objects)
            out.append(o.key);
        return out;
    }
    void remove(const QString& key)
    {
        const qsizetype i = indexOf(key);
        if (i >= 0)
            m_objects.removeAt(i);
    }

    // Keys a delete refuses (object lock, permissions).
    QSet<QString> locked;
    // Batch / single deletes and PUTs seen.
    int deleteBatches = 0;
    int singleDeletes = 0;

    // A path-style bucket: GET listing, HEAD, PUT, DELETE, POST ?delete.
    [[nodiscard]] s3::testing::FakeResponse handle(const s3::testing::RecordedRequest& req)
    {
        using s3::testing::FakeResponse;
        const QString path = QUrl::fromPercentEncoding(req.path);
        const qsizetype slash = path.indexOf(u'/', 1);
        const QString key = slash < 0 ? QString() : path.mid(slash + 1);
        if (req.method == "GET" && key.isEmpty())
            return list(req);
        if (req.method == "HEAD" && !key.isEmpty()) {
            const qsizetype i = indexOf(key);
            return i < 0 ? FakeResponse::s3Error(404, "NotFound")
                         : FakeResponse().withHeader("Content-Length", QByteArray::number(m_objects[i].size))
                               .withHeader("ETag", "\"0\"");
        }
        if (req.method == "PUT" && !key.isEmpty()) {
            remove(key);
            add(key, req.body.size());
            sort();
            return FakeResponse().withHeader("ETag", "\"0\"");
        }
        if (req.method == "DELETE" && !key.isEmpty()) {
            ++singleDeletes;
            if (locked.contains(key))
                return FakeResponse::s3Error(403, "AccessDenied", "locked");
            remove(key);
            FakeResponse r;
            r.status = 204;
            return r;
        }
        if (req.method == "POST" && req.hasQueryItem("delete")) {
            ++deleteBatches;
            QByteArray body = "<DeleteResult>";
            QXmlStreamReader xml(req.body);
            while (!xml.atEnd()) {
                if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() != QLatin1String("Key"))
                    continue;
                const QString k = xml.readElementText();
                if (locked.contains(k)) {
                    body += "<Error><Key>" + escape(k) + "</Key><Code>AccessDenied</Code><Message>locked</Message></Error>";
                } else {
                    remove(k);
                    body += "<Deleted><Key>" + escape(k) + "</Key></Deleted>";
                }
            }
            return FakeResponse::ok(body + "</DeleteResult>");
        }
        return FakeResponse::s3Error(501, "NotImplemented");
    }

    [[nodiscard]] s3::testing::FakeResponse list(const s3::testing::RecordedRequest& req) const
    {
        const auto item = [&](const QByteArray& name) {
            return QUrl::fromPercentEncoding(req.queryItem(name));
        };
        const QString prefix = item("prefix");
        const QString delimiter = item("delimiter");
        const int maxKeys = req.hasQueryItem("max-keys") ? item("max-keys").toInt() : 1000;
        qsizetype i = req.hasQueryItem("continuation-token") ? item("continuation-token").toLongLong() : 0;

        QByteArray contents;
        QByteArray prefixes;
        int count = 0;
        QString lastPrefix;
        for (; i < m_objects.size() && count < maxKeys; ++i) {
            const FakeObject& o = m_objects[i];
            if (!o.key.startsWith(prefix))
                continue;
            if (!delimiter.isEmpty()) {
                const qsizetype d = o.key.indexOf(delimiter, prefix.size());
                if (d >= 0) {
                    const QString cp = o.key.left(d + delimiter.size());
                    if (cp != lastPrefix) {
                        prefixes += "<CommonPrefixes><Prefix>" + escape(cp) + "</Prefix></CommonPrefixes>";
                        lastPrefix = cp;
                        ++count;
                    }
                    continue;
                }
            }
            contents += "<Contents><Key>" + escape(o.key)
                      + "</Key><LastModified>2026-10-20T08:00:00.000Z</LastModified><ETag>\"0\"</ETag><Size>"
                      + QByteArray::number(o.size) + "</Size><StorageClass>" + o.storageClass
                      + "</StorageClass></Contents>";
            ++count;
        }
        // Skip the rest of a common prefix the page ended in.
        while (!lastPrefix.isEmpty() && i < m_objects.size() && m_objects[i].key.startsWith(lastPrefix))
            ++i;
        bool more = false;
        for (qsizetype j = i; j < m_objects.size() && !more; ++j)
            more = m_objects[j].key.startsWith(prefix);

        QByteArray xml = "<ListBucketResult><Name>bkt</Name><Prefix>" + escape(prefix) + "</Prefix><KeyCount>"
                       + QByteArray::number(count) + "</KeyCount><MaxKeys>" + QByteArray::number(maxKeys)
                       + "</MaxKeys><IsTruncated>" + (more ? "true" : "false") + "</IsTruncated>";
        if (more)
            xml += "<NextContinuationToken>" + QByteArray::number(i) + "</NextContinuationToken>";
        xml += contents + prefixes + "</ListBucketResult>";
        return s3::testing::FakeResponse::ok(xml);
    }

private:
    [[nodiscard]] qsizetype indexOf(const QString& key) const
    {
        for (qsizetype i = 0; i < m_objects.size(); ++i) {
            if (m_objects[i].key == key)
                return i;
        }
        return -1;
    }

    static QByteArray escape(const QString& s)
    {
        return s.toHtmlEscaped().toUtf8();
    }

    QList<FakeObject> m_objects;
};

} // namespace qbucket::testing
