#include "core/ErrorMapping.h"
#include "xml/XmlParsers.h"
#include "xml/XmlWriters.h"

#include <QFile>
#include <QtTest>

using namespace qbucket::s3;

namespace {

// Fixtures in tests/data/generic/ follow the S3 API reference; provider-
// captured ones go to tests/data/<provider>/.
QByteArray fixture(const char* name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/generic/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", name);
    return f.readAll();
}

} // namespace

class TestXmlParsers : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void listV2()
    {
        const Result<ListPage> page = xml::parseListObjectsV2(fixture("list-v2.xml"), false);
        QVERIFY2(page, qPrintable(page ? QString() : page.error().toString()));
        QCOMPARE(page->bucket, QStringLiteral("bkt"));
        QCOMPARE(page->prefix, QStringLiteral("photos/"));
        QCOMPARE(page->delimiter, QStringLiteral("/"));
        QVERIFY(page->isTruncated);
        QCOMPARE(page->nextContinuationToken,
                 QStringLiteral("1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM="));
        QCOMPARE(page->objects.size(), 1);
        const ObjectEntry& e = page->objects.first();
        QCOMPARE(e.key, QStringLiteral("photos/a.jpg"));
        QCOMPARE(e.size, Q_INT64_C(5368709120));
        QCOMPARE(e.etag, QStringLiteral("\"9b2cf535f27731c974343645a3985328\""));
        QCOMPARE(e.storageClass, QStringLiteral("STANDARD"));
        QCOMPARE(e.lastModified, QDateTime(QDate(2026, 10, 20), QTime(8, 0, 1), QTimeZone::UTC));
        QCOMPARE(page->commonPrefixes, QStringList{QStringLiteral("photos/2026/")});
    }

    void listV2UrlEncoded()
    {
        const Result<ListPage> page = xml::parseListObjectsV2(fixture("list-v2-url-encoded.xml"), false);
        QVERIFY(page);
        QCOMPARE(page->prefix, QStringLiteral("a dir/"));
        QCOMPARE(page->delimiter, QStringLiteral("/"));
        QCOMPARE(page->objects.first().key, QStringLiteral("a dir/c++\nfile"));
        QCOMPARE(page->commonPrefixes.first(), QStringLiteral("a dir/✓/"));
    }

    void listPlusIsSpaceQuirk()
    {
        const QByteArray body = "<ListBucketResult><Name>b</Name><IsTruncated>false</IsTruncated>"
                                "<Contents><Key>a+b%2Bc</Key></Contents>"
                                "<EncodingType>url</EncodingType></ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV2(body, false)->objects.first().key, QStringLiteral("a+b+c"));
        QCOMPARE(xml::parseListObjectsV2(body, true)->objects.first().key, QStringLiteral("a b+c"));
    }

    void listNotEncodedKeepsPercent()
    {
        const QByteArray body = "<ListBucketResult><Name>b</Name><IsTruncated>false</IsTruncated>"
                                "<Contents><Key>100%25</Key></Contents></ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV2(body, false)->objects.first().key, QStringLiteral("100%25"));
    }

    void listErrors()
    {
        // Truncated without token: we'd loop or stop silently — both wrong.
        const QByteArray noToken = "<ListBucketResult><IsTruncated>true</IsTruncated>"
                                   "<Contents><Key>a</Key></Contents></ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV2(noToken, false).error().category, ErrorCategory::Protocol);

        // Cut off mid-document.
        QCOMPARE(xml::parseListObjectsV2(fixture("list-v2.xml").left(fixture("list-v2.xml").size() / 2), false).error().category,
                 ErrorCategory::Protocol);

        // A server that ignored the request and sent something else.
        QCOMPARE(xml::parseListObjectsV2("<ListAllMyBucketsResult/>", false).error().category,
                 ErrorCategory::Protocol);
        QCOMPARE(xml::parseListObjectsV2("not xml", false).error().category, ErrorCategory::Protocol);
        QCOMPARE(xml::parseListObjectsV2("", false).error().category, ErrorCategory::Protocol);

        const QByteArray badEscape = "<ListBucketResult><IsTruncated>false</IsTruncated>"
                                     "<Contents><Key>a%zz</Key></Contents>"
                                     "<EncodingType>url</EncodingType></ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV2(badEscape, false).error().category, ErrorCategory::Protocol);
    }

    void listV1Markers()
    {
        const QByteArray withNext = "<ListBucketResult><Name>b</Name><IsTruncated>true</IsTruncated>"
                                    "<NextMarker>m%2Fx</NextMarker>"
                                    "<Contents><Key>a</Key></Contents>"
                                    "<EncodingType>url</EncodingType></ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV1(withNext, false)->nextContinuationToken,
                 QStringLiteral("m/x"));

        const QByteArray lastKey = "<ListBucketResult><IsTruncated>true</IsTruncated>"
                                   "<Contents><Key>a</Key></Contents><Contents><Key>c</Key></Contents>"
                                   "<CommonPrefixes><Prefix>b/</Prefix></CommonPrefixes>"
                                   "</ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV1(lastKey, false)->nextContinuationToken, QStringLiteral("c"));

        const QByteArray prefixLast = "<ListBucketResult><IsTruncated>true</IsTruncated>"
                                      "<Contents><Key>a</Key></Contents>"
                                      "<CommonPrefixes><Prefix>z/</Prefix></CommonPrefixes>"
                                      "</ListBucketResult>";
        QCOMPARE(xml::parseListObjectsV1(prefixLast, false)->nextContinuationToken, QStringLiteral("z/"));

        const QByteArray empty = "<ListBucketResult><IsTruncated>true</IsTruncated></ListBucketResult>";
        QVERIFY(!xml::parseListObjectsV1(empty, false));
    }

    void listBuckets()
    {
        const QByteArray body = fixture("list-buckets.xml");
        QString token;
        const Result<BucketList> list = xml::parseListBuckets(body, &token);
        QVERIFY(list);
        QCOMPARE(list->ownerId, QStringLiteral("abc"));
        QCOMPARE(list->buckets.size(), 2);
        QCOMPARE(list->buckets[1].name, QStringLiteral("two"));
        QCOMPARE(list->buckets[1].region, QStringLiteral("fsn1"));
        QCOMPARE(list->buckets[0].creationDate,
                 QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), QTimeZone::UTC));
        QCOMPARE(token, QStringLiteral("next"));

        QString none;
        QVERIFY(xml::parseListBuckets("<ListAllMyBucketsResult><Buckets/></ListAllMyBucketsResult>",
                                      &none));
        QVERIFY(none.isEmpty());
    }

    void deleteResult()
    {
        const QByteArray body = fixture("delete-result.xml");
        const Result<DeleteObjectsResult> r = xml::parseDeleteResult(body);
        QVERIFY(r);
        QCOMPARE(r->deleted.size(), 2);
        QVERIFY(r->deleted[1].deleteMarker);
        QCOMPARE(r->deleted[1].deleteMarkerVersionId, QStringLiteral("dm"));
        QCOMPARE(r->failed.size(), 1);
        QCOMPARE(r->failed[0].code, QStringLiteral("AccessDenied"));
    }

    void locationConstraint()
    {
        QCOMPARE(xml::parseLocationConstraint(fixture("location-eu.xml"))
                     .value(),
                 QStringLiteral("EU"));
        QCOMPARE(xml::parseLocationConstraint("<LocationConstraint/>").value(), QString());
        QVERIFY(!xml::parseLocationConstraint("<Error><Code>X</Code></Error>"));
    }

    void errorBodies()
    {
        const QByteArray body = fixture("error-request-time-too-skewed.xml");
        const auto e = xml::parseErrorBody(body);
        QVERIFY(e);
        QCOMPARE(e->code, QStringLiteral("RequestTimeTooSkewed"));
        QCOMPARE(e->serverTime, QDateTime(QDate(2026, 10, 20), QTime(8, 20), QTimeZone::UTC));
        QCOMPARE(e->requestId, QStringLiteral("RID"));

        QVERIFY(!xml::parseErrorBody("<ListBucketResult/>"));
        QVERIFY(!xml::parseErrorBody(""));
        QCOMPARE(xml::rootElementName("<?xml version=\"1.0\"?><VersioningConfiguration/>"),
                 QStringLiteral("VersioningConfiguration"));
        QCOMPARE(xml::rootElementName("{\"Version\":\"2012-10-17\"}"), QString());
    }

    void errorMapping_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("category");

        const auto err = [](const char* code) {
            return QByteArray("<Error><Code>") + code + "</Code><Message>m</Message></Error>";
        };
        QTest::newRow("NoSuchKey") << 404 << err("NoSuchKey") << int(ErrorCategory::NotFound);
        QTest::newRow("HEAD 404") << 404 << QByteArray() << int(ErrorCategory::NotFound);
        QTest::newRow("NoSuchCORS") << 404 << err("NoSuchCORSConfiguration") << int(ErrorCategory::NotConfigured);
        QTest::newRow("ObjectLock") << 404 << err("ObjectLockConfigurationNotFoundError") << int(ErrorCategory::NotConfigured);
        QTest::newRow("AccessDenied") << 403 << err("AccessDenied") << int(ErrorCategory::NotPermitted);
        QTest::newRow("HEAD 403") << 403 << QByteArray() << int(ErrorCategory::NotPermitted);
        QTest::newRow("bad signature") << 403 << err("SignatureDoesNotMatch") << int(ErrorCategory::Authentication);
        QTest::newRow("bad key") << 403 << err("InvalidAccessKeyId") << int(ErrorCategory::Authentication);
        QTest::newRow("skew") << 403 << err("RequestTimeTooSkewed") << int(ErrorCategory::ClockSkew);
        QTest::newRow("not implemented") << 501 << err("NotImplemented") << int(ErrorCategory::Unsupported);
        QTest::newRow("bare 501") << 501 << QByteArray() << int(ErrorCategory::Unsupported);
        QTest::newRow("405 is not unsupported") << 405 << err("MethodNotAllowed") << int(ErrorCategory::InvalidRequest);
        QTest::newRow("SlowDown") << 503 << err("SlowDown") << int(ErrorCategory::Transient);
        QTest::newRow("bare 502") << 502 << QByteArray("<html>bad gateway</html>") << int(ErrorCategory::Transient);
        QTest::newRow("RequestTimeout 400") << 400 << err("RequestTimeout") << int(ErrorCategory::Transient);
        QTest::newRow("412") << 412 << err("PreconditionFailed") << int(ErrorCategory::PreconditionFailed);
        QTest::newRow("BucketNotEmpty") << 409 << err("BucketNotEmpty") << int(ErrorCategory::Conflict);
        QTest::newRow("redirect") << 301 << err("PermanentRedirect") << int(ErrorCategory::Redirect);
        QTest::newRow("wrong region") << 400 << err("AuthorizationHeaderMalformed") << int(ErrorCategory::Redirect);
        QTest::newRow("unknown code 400") << 400 << err("WeirdCode") << int(ErrorCategory::InvalidRequest);
    }

    void errorMapping()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, category);
        const Error e = errorFromResponse(status, body, QStringLiteral("hdr-rid"));
        QCOMPARE(int(e.category), category);
        QCOMPARE(e.httpStatus, status);
        QVERIFY(!e.message.isEmpty());
        QVERIFY(!e.requestId.isEmpty());
    }

    void deleteRequestBody()
    {
        const Result<QByteArray> body = xml::buildDeleteRequest(
            {{QStringLiteral("a&b<c>"), QString()},
             {QStringLiteral("line\r\nbreak\t\"q'"), QStringLiteral("v1")}},
            true);
        QVERIFY(body);
        QVERIFY(body->contains("<Quiet>true</Quiet>"));
        QVERIFY(body->contains("<Key>a&amp;b&lt;c&gt;</Key>"));
        QVERIFY(body->contains("<Key>line&#13;&#10;break&#9;&quot;q&apos;</Key><VersionId>v1</VersionId>"));

        // Round trip through a real XML parser keeps the key byte-exact.
        QXmlStreamReader r(*body);
        QStringList keys;
        while (!r.atEnd()) {
            if (r.readNextStartElement() && r.name() == QLatin1String("Key"))
                keys.append(r.readElementText());
        }
        QCOMPARE(keys, (QStringList{QStringLiteral("a&b<c>"), QStringLiteral("line\r\nbreak\t\"q'")}));

        const Result<QByteArray> bad =
            xml::buildDeleteRequest({{QStringLiteral("bell\x07"), QString()}}, false);
        QCOMPARE(bad.error().category, ErrorCategory::InvalidRequest);
    }
};

QTEST_GUILESS_MAIN(TestXmlParsers)
#include "test_xml_parsers.moc"
