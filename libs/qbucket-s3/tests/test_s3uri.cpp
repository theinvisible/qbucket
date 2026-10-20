#include "http/S3Uri.h"
#include "http/UriEncoding.h"

#include <QtTest>

using namespace qbucket::s3;

namespace {

Endpoint endpoint(const char* url)
{
    Endpoint e;
    e.url = QUrl(QString::fromLatin1(url));
    e.region = QStringLiteral("test-1");
    return e;
}

} // namespace

// The request target is what gets signed — and QUrl must hand exactly these
// bytes to the wire. Every row checks both.
class TestS3Uri : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void encodesKeysInPaths_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QByteArray>("path");

        QTest::newRow("plain") << QStringLiteral("file.txt") << QByteArray("/bkt/file.txt");
        QTest::newRow("unreserved") << QStringLiteral("a-b_c.d~e") << QByteArray("/bkt/a-b_c.d~e");
        QTest::newRow("space") << QStringLiteral("a b") << QByteArray("/bkt/a%20b");
        QTest::newRow("plus") << QStringLiteral("a+b") << QByteArray("/bkt/a%2Bb");
        QTest::newRow("percent") << QStringLiteral("100%") << QByteArray("/bkt/100%25");
        QTest::newRow("encoded-looking") << QStringLiteral("a%2Fb") << QByteArray("/bkt/a%252Fb");
        QTest::newRow("query chars") << QStringLiteral("q?x#y") << QByteArray("/bkt/q%3Fx%23y");
        QTest::newRow("sub-delims")
            << QStringLiteral("!$&'()*,;=:@")
            << QByteArray("/bkt/%21%24%26%27%28%29%2A%2C%3B%3D%3A%40");
        QTest::newRow("brackets etc")
            << QStringLiteral("[]{}<>|\\^`\"")
            << QByteArray("/bkt/%5B%5D%7B%7D%3C%3E%7C%5C%5E%60%22");
        QTest::newRow("control chars") << QStringLiteral("a\tb\nc") << QByteArray("/bkt/a%09b%0Ac");
        QTest::newRow("folder marker") << QStringLiteral("dir/") << QByteArray("/bkt/dir/");
        QTest::newRow("nested") << QStringLiteral("a/b/c.txt") << QByteArray("/bkt/a/b/c.txt");
        QTest::newRow("double slash") << QStringLiteral("dir//file") << QByteArray("/bkt/dir//file");
        QTest::newRow("leading slash") << QStringLiteral("/lead") << QByteArray("/bkt//lead");
        QTest::newRow("dots in names") << QStringLiteral("a.b/.../.hidden/x.")
                                       << QByteArray("/bkt/a.b/.../.hidden/x.");
        QTest::newRow("unicode NFC") << QStringLiteral("café") << QByteArray("/bkt/caf%C3%A9");
        QTest::newRow("unicode NFD") << QStringLiteral("café") << QByteArray("/bkt/cafe%CC%81");
        QTest::newRow("emoji") << QStringLiteral("\U0001F600.png") << QByteArray("/bkt/%F0%9F%98%80.png");
        QTest::newRow("cjk") << QStringLiteral("文件") << QByteArray("/bkt/%E6%96%87%E4%BB%B6");
        QTest::newRow("1024 bytes") << QString(1024, QLatin1Char('k'))
                                    << QByteArray("/bkt/") + QByteArray(1024, 'k');
    }

    void encodesKeysInPaths()
    {
        QFETCH(QString, key);
        QFETCH(QByteArray, path);

        const Result<S3Uri> uri = S3Uri::build(endpoint("https://s3.example.com"),
                                               AddressingStyle::Path, QStringLiteral("bkt"), key);
        QVERIFY2(uri, qPrintable(uri ? QString() : uri.error().toString()));
        QCOMPARE(uri->canonicalUri(), path);
        QCOMPARE(uri->url().toEncoded(), QByteArray("https://s3.example.com") + path);

        // The same key virtual-hosted: path without the bucket.
        const Result<S3Uri> vh = S3Uri::build(endpoint("https://s3.example.com"),
                                              AddressingStyle::VirtualHosted,
                                              QStringLiteral("bkt"), key);
        QVERIFY(vh);
        QCOMPARE(vh->host(), QByteArray("bkt.s3.example.com"));
        QCOMPARE(vh->canonicalUri(), path.mid(4));
        QCOMPARE(vh->url().toEncoded(), QByteArray("https://bkt.s3.example.com") + path.mid(4));
    }

    void refusesDotSegments_data()
    {
        QTest::addColumn<QString>("key");
        QTest::newRow("dot") << QStringLiteral("./x");
        QTest::newRow("dotdot") << QStringLiteral("a/../x");
        QTest::newRow("only dots") << QStringLiteral("..");
        QTest::newRow("trailing dot") << QStringLiteral("dir/.");
        QTest::newRow("folder marker") << QStringLiteral("../");
    }

    // QNAM normalises these away on the wire (verified: "/bkt/a/../b" goes
    // out as "/bkt/b"), so the request would hit a different object.
    void refusesDotSegments()
    {
        QFETCH(QString, key);
        const Result<S3Uri> uri = S3Uri::build(endpoint("https://s3.example.com"),
                                               AddressingStyle::Path, QStringLiteral("bkt"), key);
        QVERIFY(!uri);
        QCOMPARE(uri.error().category, ErrorCategory::InvalidRequest);
    }

    void serviceAndBucketLevel()
    {
        const auto svc = S3Uri::build(endpoint("https://s3.example.com"),
                                      AddressingStyle::VirtualHosted, QString(), QString());
        QVERIFY(svc);
        QCOMPARE(svc->canonicalUri(), QByteArray("/"));
        QCOMPARE(svc->host(), QByteArray("s3.example.com"));

        const auto path = S3Uri::build(endpoint("https://s3.example.com"), AddressingStyle::Path,
                                       QStringLiteral("bkt"), QString());
        QVERIFY(path);
        QCOMPARE(path->canonicalUri(), QByteArray("/bkt"));

        const auto vh = S3Uri::build(endpoint("https://s3.example.com"),
                                     AddressingStyle::VirtualHosted, QStringLiteral("bkt"),
                                     QString());
        QVERIFY(vh);
        QCOMPARE(vh->canonicalUri(), QByteArray("/"));
        QCOMPARE(vh->host(), QByteArray("bkt.s3.example.com"));

        QVERIFY(!S3Uri::build(endpoint("https://s3.example.com"), AddressingStyle::Path,
                              QString(), QStringLiteral("key")));
    }

    void queryIsSortedAndEncoded()
    {
        const QueryParams q = {
            {"prefix", "a b/", false},
            {"list-type", "2", false},
            {"delimiter", "/", false},
            {"continuation-token", "1/2+3==", false},
            {"encoding-type", "url", false},
        };
        const auto uri = S3Uri::build(endpoint("https://s3.example.com"), AddressingStyle::Path,
                                      QStringLiteral("bkt"), QString(), q);
        QVERIFY(uri);
        const QByteArray expected =
            "continuation-token=1%2F2%2B3%3D%3D&delimiter=%2F&encoding-type=url"
            "&list-type=2&prefix=a%20b%2F";
        QCOMPARE(uri->canonicalQuery(), expected);
        QCOMPARE(uri->encodedQuery(), expected);
        QCOMPARE(uri->url().toEncoded(), QByteArray("https://s3.example.com/bkt?") + expected);
    }

    void bareSubresources()
    {
        const QueryParams q = {{"versioning", {}, true}};
        const auto uri = S3Uri::build(endpoint("https://s3.example.com"), AddressingStyle::Path,
                                      QStringLiteral("bkt"), QString(), q);
        QVERIFY(uri);
        QCOMPARE(uri->encodedQuery(), QByteArray("versioning"));
        QCOMPARE(uri->canonicalQuery(), QByteArray("versioning="));

        // Same name twice sorts by value.
        const QueryParams dup = {{"x", "b", false}, {"x", "a", false}, {"uploads", {}, true}};
        const auto d = S3Uri::build(endpoint("https://s3.example.com"), AddressingStyle::Path,
                                    QStringLiteral("bkt"), QStringLiteral("k"), dup);
        QVERIFY(d);
        QCOMPARE(d->encodedQuery(), QByteArray("uploads&x=a&x=b"));
        QCOMPARE(d->canonicalQuery(), QByteArray("uploads=&x=a&x=b"));
    }

    void addressingFallbacks_data()
    {
        QTest::addColumn<QString>("endpointUrl");
        QTest::addColumn<int>("preferred");
        QTest::addColumn<QString>("bucket");
        QTest::addColumn<int>("expected");

        const int path = int(AddressingStyle::Path);
        const int vh = int(AddressingStyle::VirtualHosted);
        const int autoStyle = int(AddressingStyle::Auto);
        QTest::newRow("auto plain") << "https://s3.example.com" << autoStyle << "bkt" << vh;
        QTest::newRow("explicit path") << "https://s3.example.com" << path << "bkt" << path;
        QTest::newRow("dotted over tls") << "https://s3.example.com" << vh << "a.b.c" << path;
        QTest::newRow("dotted over http") << "http://s3.example.com" << vh << "a.b.c" << vh;
        QTest::newRow("upper case") << "https://s3.example.com" << vh << "MyBucket" << path;
        QTest::newRow("underscore") << "https://s3.example.com" << vh << "my_bucket" << path;
        QTest::newRow("too short") << "https://s3.example.com" << vh << "ab" << path;
        QTest::newRow("ip-like name") << "https://s3.example.com" << vh << "192.168.1.1" << path;
        QTest::newRow("ip endpoint") << "http://127.0.0.1:9000" << autoStyle << "bkt" << path;
        QTest::newRow("localhost") << "http://localhost:7480" << vh << "bkt" << path;
        QTest::newRow("ipv6 endpoint") << "http://[::1]:7480" << vh << "bkt" << path;
    }

    void addressingFallbacks()
    {
        QFETCH(QString, endpointUrl);
        QFETCH(int, preferred);
        QFETCH(QString, bucket);
        QFETCH(int, expected);
        QCOMPARE(int(S3Uri::effectiveStyle(AddressingStyle(preferred), bucket, QUrl(endpointUrl))),
                 expected);
    }

    void hostHeader_data()
    {
        QTest::addColumn<QString>("endpointUrl");
        QTest::addColumn<QByteArray>("host");
        QTest::addColumn<QByteArray>("encodedUrl");

        QTest::newRow("default https port dropped")
            << "https://S3.Example.com:443" << QByteArray("s3.example.com")
            << QByteArray("https://s3.example.com/bkt/k");
        QTest::newRow("default http port dropped")
            << "http://s3.example.com:80" << QByteArray("s3.example.com")
            << QByteArray("http://s3.example.com/bkt/k");
        QTest::newRow("custom port kept")
            << "http://localhost:7480" << QByteArray("localhost:7480")
            << QByteArray("http://localhost:7480/bkt/k");
        QTest::newRow("ipv6")
            << "http://[::1]:7480" << QByteArray("[::1]:7480")
            << QByteArray("http://[::1]:7480/bkt/k");
    }

    void hostHeader()
    {
        QFETCH(QString, endpointUrl);
        QFETCH(QByteArray, host);
        QFETCH(QByteArray, encodedUrl);
        Endpoint e;
        e.url = QUrl(endpointUrl);
        e.region = QStringLiteral("r");
        const auto uri = S3Uri::build(e, AddressingStyle::Path, QStringLiteral("bkt"),
                                      QStringLiteral("k"));
        QVERIFY2(uri, qPrintable(uri ? QString() : uri.error().toString()));
        QCOMPARE(uri->host(), host);
        QCOMPARE(uri->url().toEncoded(), encodedUrl);
    }

    void endpointValidation()
    {
        Endpoint e = endpoint("https://s3.example.com");
        QVERIFY(e.isValid());
        QVERIFY(e.isHttps());

        QVERIFY(!endpoint("ftp://s3.example.com").isValid());
        QVERIFY(!endpoint("https://s3.example.com/base").isValid());
        QVERIFY(!endpoint("https://user:pw@s3.example.com").isValid());
        QVERIFY(!endpoint("https://s3.example.com?x=1").isValid());
        QVERIFY(endpoint("https://s3.example.com/").isValid());

        e.region.clear();
        QVERIFY(!e.isValid());
        QVERIFY(!S3Uri::build(e, AddressingStyle::Path, QStringLiteral("bkt"), QString()));
    }

    void decodeRoundTrip()
    {
        const QString key = QStringLiteral("a b+c%d/é\U0001F600");
        const QByteArray enc = uriEncodeText(key, SlashMode::Encode);
        QByteArray dec;
        QVERIFY(uriDecode(enc, false, &dec));
        QCOMPARE(QString::fromUtf8(dec), key);

        QVERIFY(uriDecode("a+b", false, &dec));
        QCOMPARE(dec, QByteArray("a+b"));
        QVERIFY(uriDecode("a+b", true, &dec));
        QCOMPARE(dec, QByteArray("a b"));

        QVERIFY(!uriDecode("%", false, &dec));
        QVERIFY(!uriDecode("%4", false, &dec));
        QVERIFY(!uriDecode("%zz", false, &dec));
    }
};

QTEST_GUILESS_MAIN(TestS3Uri)
#include "test_s3uri.moc"
