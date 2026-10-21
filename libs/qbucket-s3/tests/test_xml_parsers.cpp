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

    // --- multipart --------------------------------------------------------------

    void initiateMultipartUpload()
    {
        const auto r = xml::parseInitiateMultipartUpload(fixture("initiate-multipart-upload.xml"));
        QVERIFY(r);
        QCOMPARE(r->bucket, QStringLiteral("example-bucket"));
        QCOMPARE(r->key, QStringLiteral("example-object"));
        QCOMPARE(r->uploadId, QStringLiteral("VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA"));
        QVERIFY(!xml::parseInitiateMultipartUpload("<InitiateMultipartUploadResult/>"));
    }

    void completeMultipartUpload()
    {
        const auto r = xml::parseCompleteMultipartUpload(fixture("complete-multipart-upload.xml"));
        QVERIFY(r);
        QCOMPARE(r->etag, QStringLiteral("\"3858f62230ac3c915f300c664312c11f-9\""));
        QVERIFY(r->location.endsWith(QStringLiteral("/Example-Object")));
        // A 200 with an <Error> body is never a success.
        const auto e = xml::parseCompleteMultipartUpload(fixture("complete-multipart-upload-error.xml"));
        QVERIFY(!e);
        QCOMPARE(e.error().category, ErrorCategory::Protocol);
        QCOMPARE(xml::rootElementName(fixture("complete-multipart-upload-error.xml")), QStringLiteral("Error"));
    }

    void listParts()
    {
        const auto r = xml::parseListParts(fixture("list-parts.xml"));
        QVERIFY(r);
        QCOMPARE(r->parts.size(), 2);
        QCOMPARE(r->parts[0].partNumber, 2);
        QCOMPARE(r->parts[0].size, 10485760);
        QCOMPARE(r->parts[0].etag, QStringLiteral("\"7778aef83f66abc1fa1e8477f296d394\""));
        QVERIFY(r->parts[1].lastModified.isValid());
        QVERIFY(r->isTruncated);
        QCOMPARE(r->nextPartNumberMarker, 3);
        QCOMPARE(r->storageClass, QStringLiteral("STANDARD"));

        // Truncated without a marker: continue after the last part.
        const auto noMarker = xml::parseListParts(
            "<ListPartsResult><IsTruncated>true</IsTruncated><Part><PartNumber>7</PartNumber><ETag>\"x\"</ETag>"
            "<Size>1</Size></Part></ListPartsResult>");
        QVERIFY(noMarker);
        QCOMPARE(noMarker->nextPartNumberMarker, 7);
        QVERIFY(!xml::parseListParts("<ListPartsResult><IsTruncated>true</IsTruncated></ListPartsResult>"));
        QVERIFY(!xml::parseListParts("<ListPartsResult><Part><PartNumber>0</PartNumber></Part></ListPartsResult>"));
    }

    void listMultipartUploads()
    {
        const auto r = xml::parseListMultipartUploads(fixture("list-multipart-uploads.xml"), false);
        QVERIFY(r);
        QCOMPARE(r->uploads.size(), 2);
        QCOMPARE(r->uploads[0].key, QStringLiteral("my-divisor"));
        QCOMPARE(r->uploads[1].key, QStringLiteral("my movie.m2ts"));   // url-decoded
        QCOMPARE(r->uploads[1].uploadId, QStringLiteral("YW55IGlkZWEgd2h5IGVsdmluZydzIHVwbG9hZCBmYWlsZWQ"));
        QCOMPARE(r->uploads[0].initiator, QStringLiteral("user1-11111a31-17b5-4fb7-9df5-b111111f13de"));
        QVERIFY(r->uploads[0].initiated.isValid());
        QVERIFY(r->isTruncated);
        QCOMPARE(r->nextKeyMarker, QStringLiteral("my movie.m2ts"));

        // Hetzner's answer for a bucket without unfinished uploads.
        QFile f(QStringLiteral(TEST_DATA_DIR "/hetzner/probe-uploads-200.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto hetzner = xml::parseListMultipartUploads(f.readAll(), false);
        QVERIFY(hetzner);
        QVERIFY(hetzner->uploads.isEmpty());
        QVERIFY(!hetzner->isTruncated);
    }

    void listVersions()
    {
        const auto r = xml::parseListVersions(fixture("list-versions.xml"), false);
        QVERIFY(r);
        QCOMPARE(r->bucket, QStringLiteral("bucket"));
        QCOMPARE(r->prefix, QStringLiteral("docs/"));
        QCOMPARE(r->delimiter, QStringLiteral("/"));
        QCOMPARE(r->entries.size(), 3);   // versions and delete markers in document order
        const VersionEntry& a = r->entries[0];
        QCOMPARE(a.key, QStringLiteral("docs/a.txt"));
        QCOMPARE(a.versionId, QStringLiteral("null"));
        QVERIFY(a.isLatest && !a.isDeleteMarker);
        QCOMPARE(a.size, 434234);
        QCOMPARE(a.etag, QStringLiteral("\"fba9dede5f27731c9771645a39863328\""));
        QCOMPARE(a.storageClass, QStringLiteral("STANDARD"));
        QVERIFY(a.lastModified.isValid());
        const VersionEntry& marker = r->entries[1];
        QCOMPARE(marker.key, QStringLiteral("docs/my file.txt"));   // url-decoded
        QVERIFY(marker.isDeleteMarker && marker.isLatest);
        QCOMPARE(marker.size, 0);
        QVERIFY(marker.etag.isEmpty());
        const VersionEntry& older = r->entries[2];
        QVERIFY(!older.isLatest && !older.isDeleteMarker);
        // Version ids are never url-encoded.
        QCOMPARE(older.versionId, QStringLiteral("3/L4kqtJlcpXroDTDmJ+rmSpXd3dIbrHY+MTRCxf3vjVBH40Nr8X8gdRQBpUMLUo"));
        QCOMPARE(r->commonPrefixes, QStringList{QStringLiteral("docs/old+/")});
        QVERIFY(r->isTruncated);
        QCOMPARE(r->nextKeyMarker, QStringLiteral("docs/my file.txt"));
        QCOMPARE(r->nextVersionIdMarker, older.versionId);

        // Truncated without NextKeyMarker: continue after the last entry.
        const auto noMarker = xml::parseListVersions(fixture("list-versions-no-next-marker.xml"), false);
        QVERIFY(noMarker);
        QCOMPARE(noMarker->nextKeyMarker, QStringLiteral("a"));
        QCOMPARE(noMarker->nextVersionIdMarker, QStringLiteral("v1"));

        // VersityGW: every marker of the page before every version — put in
        // S3's order (by key, the current entry first, then newest first).
        const auto grouped = xml::parseListVersions(fixture("list-versions-markers-first.xml"), false);
        QVERIFY(grouped);
        QStringList order;
        for (const VersionEntry& e : grouped->entries)
            order.append(e.versionId);
        QCOMPARE(order, (QStringList{QStringLiteral("a-marker"), QStringLiteral("a-v1"), QStringLiteral("b-v2"),
                                     QStringLiteral("b-old-marker"), QStringLiteral("b-v1")}));

        // Hetzner's answer to the capability probe (max-keys=1, empty bucket).
        QFile f(QStringLiteral(TEST_DATA_DIR "/hetzner/probe-versions-200.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto hetzner = xml::parseListVersions(f.readAll(), false);
        QVERIFY(hetzner);
        QVERIFY(hetzner->entries.isEmpty());
        QVERIFY(!hetzner->isTruncated);

        QVERIFY(!xml::parseListVersions(fixture("list-v2.xml"), false));   // wrong root
        QVERIFY(!xml::parseListVersions("<ListVersionsResult><Version><Key>a</Key></Version></ListVersionsResult>",
                                        false));   // no VersionId
    }

    void copyPartResult()
    {
        const auto r = xml::parseCopyPartResult(fixture("copy-part-result.xml"));
        QVERIFY(r);
        QCOMPARE(r->etag, QStringLiteral("\"9b2cf535f27731c974343645a3985328\""));
        QVERIFY(r->lastModified.isValid());
        QVERIFY(!xml::parseCopyPartResult(fixture("copy-object-result.xml")));   // wrong root
    }

    void completeRequestBody()
    {
        const QByteArray body = xml::buildCompleteMultipartUpload({{1, QStringLiteral("\"a\"")}, {2, QStringLiteral("\"b&c\"")}});
        QVERIFY(body.contains("<Part><PartNumber>1</PartNumber><ETag>&quot;a&quot;</ETag></Part>"));
        QVERIFY(body.contains("<PartNumber>2</PartNumber><ETag>&quot;b&amp;c&quot;</ETag>"));
        QVERIFY(body.startsWith("<?xml"));
    }

    void tagging()
    {
        const Result<TagSet> tags = xml::parseTagging(fixture("get-object-tagging.xml"));
        QVERIFY(tags);
        QCOMPARE(tags->size(), 2);
        QCOMPARE(tags->at(0), (Tag{QStringLiteral("project"), QStringLiteral("qbucket & co")}));
        QCOMPARE(tags->at(1), (Tag{QStringLiteral("stage"), QString()}));
        QVERIFY(!xml::parseTagging(fixture("copy-part-result.xml")));   // wrong root: never "no tags"

        // Written and read back.
        const TagSet set{{QStringLiteral("a<b"), QStringLiteral("x\ny")}, {QStringLiteral("ü"), QStringLiteral("é")}};
        const Result<QByteArray> body = xml::buildTagging(set);
        QVERIFY(body);
        QCOMPARE(*xml::parseTagging(*body), set);
        QVERIFY(xml::buildTagging({}));   // an empty set removes all tags
        QCOMPARE(xml::buildTagging({{QStringLiteral("k\x01"), QString()}}).error().category, ErrorCategory::InvalidRequest);

        QVERIFY(tagSetProblem(set).isEmpty());
        QVERIFY(!tagSetProblem({{QStringLiteral("aws:created"), QString()}}).isEmpty());
        QVERIFY(!tagSetProblem({{QString(), QStringLiteral("v")}}).isEmpty());
        QVERIFY(!tagSetProblem({{QStringLiteral("k"), QString()}, {QStringLiteral("k"), QString()}}).isEmpty());
        TagSet eleven;
        for (int i = 0; i < 11; ++i)
            eleven.append({QString::number(i), QString()});
        QVERIFY(!tagSetProblem(eleven).isEmpty());
    }

    void accessControlPolicy()
    {
        const Result<AccessControlPolicy> acl = xml::parseAccessControlPolicy(fixture("get-object-acl-public-read.xml"));
        QVERIFY(acl);
        QCOMPARE(acl->ownerName, QStringLiteral("owner"));
        QCOMPARE(acl->grants.size(), 2);
        QCOMPARE(acl->grants[0].grantee, Grant::Grantee::CanonicalUser);
        QCOMPARE(acl->grants[0].id, acl->ownerId);
        QCOMPARE(acl->grants[1].grantee, Grant::Grantee::Group);
        QCOMPARE(acl->grants[1].permission, QStringLiteral("READ"));
        QCOMPARE(cannedAclOf(*acl), std::optional<QString>(QStringLiteral("public-read")));
        QVERIFY(!xml::parseAccessControlPolicy(fixture("get-object-tagging.xml")));

        AccessControlPolicy p = *acl;
        p.grants.removeLast();
        QCOMPARE(cannedAclOf(p), std::optional<QString>(QStringLiteral("private")));
        Grant write = acl->grants[1];
        write.permission = QStringLiteral("WRITE");
        p.grants = {acl->grants[0], acl->grants[1], write};
        QCOMPARE(cannedAclOf(p), std::optional<QString>(QStringLiteral("public-read-write")));
        // Another account's grant can't be expressed as a canned ACL.
        Grant other = acl->grants[0];
        other.id = QStringLiteral("someone-else");
        other.permission = QStringLiteral("READ");
        p.grants = {acl->grants[0], other};
        QVERIFY(!cannedAclOf(p));
        // Without the owner's FULL_CONTROL it isn't canned either.
        p.grants = {acl->grants[1]};
        QVERIFY(!cannedAclOf(p));
    }
};

QTEST_GUILESS_MAIN(TestXmlParsers)
#include "test_xml_parsers.moc"
