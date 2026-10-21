#include "http/S3Uri.h"
#include "http/UriEncoding.h"
#include "signing/SigV4Signer.h"

#include <qbucket/s3/Presigner.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <algorithm>

using namespace qbucket::s3;

namespace {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

QString suiteDir()
{
    return QStringLiteral(TEST_DATA_DIR "/sigv4/aws-c-auth/v4");
}

// One request.txt of the AWS suite: request line, headers (continuation
// lines start with whitespace), blank line, body.
struct SuiteRequest {
    QByteArray method;
    QByteArray path;
    QByteArray query;
    HeaderList headers;
    QByteArray body;
};

SuiteRequest parseRequest(const QByteArray& raw)
{
    SuiteRequest r;
    const qsizetype bodySep = raw.indexOf("\n\n");
    const QByteArray head = bodySep < 0 ? raw : raw.left(bodySep);
    if (bodySep >= 0)
        r.body = raw.mid(bodySep + 2);

    const QList<QByteArray> lines = head.split('\n');
    // "GET /path?query HTTP/1.1" — the path itself may contain spaces.
    const QByteArray& requestLine = lines.first();
    const qsizetype firstSpace = requestLine.indexOf(' ');
    const qsizetype lastSpace = requestLine.lastIndexOf(' ');
    r.method = requestLine.left(firstSpace);
    const QByteArray target = requestLine.mid(firstSpace + 1, lastSpace - firstSpace - 1);
    const qsizetype q = target.indexOf('?');
    r.path = q < 0 ? target : target.left(q);
    r.query = q < 0 ? QByteArray() : target.mid(q + 1);

    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray& line = lines[i];
        if (line.isEmpty())
            continue;
        if ((line.startsWith(' ') || line.startsWith('\t')) && !r.headers.isEmpty()) {
            r.headers.last().second += ' ' + line.trimmed();
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        r.headers.append({line.left(colon), line.mid(colon + 1)});
    }
    return r;
}

// Decode the query as written in request.txt, re-encode the SigV4 way, sort.
QByteArray canonicalQueryOf(const QByteArray& query)
{
    if (query.isEmpty())
        return {};
    QList<QPair<QByteArray, QByteArray>> pairs;
    for (const QByteArray& part : query.split('&')) {
        const qsizetype eq = part.indexOf('=');
        QByteArray name;
        QByteArray value;
        uriDecode(eq < 0 ? part : part.left(eq), false, &name);
        if (eq >= 0)
            uriDecode(part.mid(eq + 1), false, &value);
        pairs.append({uriEncode(name, SlashMode::Encode), uriEncode(value, SlashMode::Encode)});
    }
    std::sort(pairs.begin(), pairs.end());
    QByteArray out;
    for (const auto& [n, v] : pairs) {
        if (!out.isEmpty())
            out += '&';
        out += n + '=' + v;
    }
    return out;
}

SigningContext s3DocContext()
{
    SigningContext ctx;
    ctx.credentials.accessKeyId = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
    ctx.credentials.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    ctx.region = "us-east-1";
    ctx.service = "s3";
    ctx.time = QDateTime(QDate(2013, 5, 24), QTime(0, 0), QTimeZone::UTC);
    return ctx;
}

Endpoint awsEndpoint()
{
    Endpoint e;
    e.url = QUrl(QStringLiteral("https://s3.amazonaws.com"));
    e.region = QStringLiteral("us-east-1");
    e.addressing = AddressingStyle::VirtualHosted;
    return e;
}

SigningInput s3Input(const QByteArray& method, const QString& key, const QueryParams& query,
                     const HeaderList& extraHeaders, const QByteArray& payloadHash)
{
    const Result<S3Uri> uri = S3Uri::build(awsEndpoint(), AddressingStyle::VirtualHosted,
                                           QStringLiteral("examplebucket"), key, query);
    Q_ASSERT(uri);
    SigningInput in;
    in.method = method;
    in.canonicalUri = uri->canonicalUri();
    in.canonicalQuery = uri->canonicalQuery();
    in.headers = HeaderList{{"Host", uri->host()}} + extraHeaders;
    in.payloadHash = payloadHash;
    return in;
}

QByteArray signatureOf(const SigningResult& r)
{
    for (const auto& [name, value] : r.addedHeaders) {
        if (name == "Authorization")
            return value.mid(value.lastIndexOf('=') + 1);
    }
    return {};
}

} // namespace

class TestSigV4 : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- AWS SigV4 test suite (generic service) -----------------------------
    void awsSuite_data()
    {
        QTest::addColumn<QString>("caseDir");
        const QStringList cases = QDir(suiteDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY2(cases.size() >= 30, "SigV4 test vectors missing");
        int used = 0;
        for (const QString& name : cases) {
            // Path normalisation is not S3 behaviour.
            if (name.endsWith(QLatin1String("-normalized")))
                continue;
            const QJsonObject ctx =
                QJsonDocument::fromJson(readFile(suiteDir() + u'/' + name + u"/context.json")).object();
            // Token added after signing: not how S3 works.
            if (ctx.value(QLatin1String("omit_session_token")).toBool())
                continue;
            QTest::newRow(qPrintable(name)) << suiteDir() + u'/' + name;
            ++used;
        }
        QVERIFY(used >= 25);
    }

    void awsSuite()
    {
        QFETCH(QString, caseDir);
        const QJsonObject json =
            QJsonDocument::fromJson(readFile(caseDir + u"/context.json")).object();
        const QJsonObject creds = json.value(QLatin1String("credentials")).toObject();

        SigningContext ctx;
        ctx.credentials.accessKeyId = creds.value(QLatin1String("access_key_id")).toString();
        ctx.credentials.secretAccessKey = creds.value(QLatin1String("secret_access_key")).toString();
        ctx.credentials.sessionToken = creds.value(QLatin1String("token")).toString();
        ctx.region = json.value(QLatin1String("region")).toString().toUtf8();
        ctx.service = json.value(QLatin1String("service")).toString().toUtf8();
        ctx.time = QDateTime::fromString(json.value(QLatin1String("timestamp")).toString(),
                                         Qt::ISODate);
        QVERIFY(ctx.time.isValid());
        const bool signBody = json.value(QLatin1String("sign_body")).toBool();
        const int expires = json.value(QLatin1String("expiration_in_seconds")).toInt();

        const SuiteRequest req = parseRequest(readFile(caseDir + u"/request.txt"));
        SigningInput in;
        in.method = req.method;
        in.canonicalUri = uriEncode(req.path, SlashMode::Keep);
        in.canonicalQuery = canonicalQueryOf(req.query);
        in.headers = req.headers;
        in.payloadHash = SigV4Signer::sha256Hex(req.body);

        const SigningResult header = SigV4Signer::signHeaders(in, ctx, signBody);
        QCOMPARE(header.canonicalRequest, readFile(caseDir + u"/header-canonical-request.txt"));
        QCOMPARE(header.stringToSign, readFile(caseDir + u"/header-string-to-sign.txt"));
        QCOMPARE(header.signature, readFile(caseDir + u"/header-signature.txt"));
        QCOMPARE(signatureOf(header), header.signature);

        const SigningResult query = SigV4Signer::presign(in, ctx, expires);
        QCOMPARE(query.canonicalRequest, readFile(caseDir + u"/query-canonical-request.txt"));
        QCOMPARE(query.stringToSign, readFile(caseDir + u"/query-string-to-sign.txt"));
        QCOMPARE(query.signature, readFile(caseDir + u"/query-signature.txt"));
    }

    // --- S3 documentation examples ------------------------------------------
    // https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
    void s3DocGetObject()
    {
        const SigningInput in = s3Input("GET", QStringLiteral("test.txt"), {},
                                        {{"Range", "bytes=0-9"}}, SigV4Signer::kEmptyPayloadHash);
        const SigningResult r = SigV4Signer::signHeaders(in, s3DocContext(), true);
        QCOMPARE(r.signature,
                 QByteArray("f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
        QVERIFY(r.canonicalRequest.contains("\nhost;range;x-amz-content-sha256;x-amz-date\n"));
    }

    void s3DocPutObject()
    {
        const QByteArray body = "Welcome to Amazon S3.";
        const QByteArray hash = SigV4Signer::sha256Hex(body);
        QCOMPARE(hash,
                 QByteArray("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));
        const SigningInput in = s3Input("PUT", QStringLiteral("test$file.text"), {},
                                        {{"Date", "Fri, 24 May 2013 00:00:00 GMT"},
                                         {"x-amz-storage-class", "REDUCED_REDUNDANCY"}},
                                        hash);
        QCOMPARE(in.canonicalUri, QByteArray("/test%24file.text"));
        const SigningResult r = SigV4Signer::signHeaders(in, s3DocContext(), true);
        QCOMPARE(r.signature,
                 QByteArray("98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"));
    }

    void s3DocGetBucketLifecycle()
    {
        const SigningInput in = s3Input("GET", QString(), {{"lifecycle", {}, true}}, {},
                                        SigV4Signer::kEmptyPayloadHash);
        QCOMPARE(in.canonicalQuery, QByteArray("lifecycle="));
        const SigningResult r = SigV4Signer::signHeaders(in, s3DocContext(), true);
        QCOMPARE(r.signature,
                 QByteArray("fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543"));
    }

    void s3DocListObjects()
    {
        const SigningInput in = s3Input("GET", QString(),
                                        {{"max-keys", "2", false}, {"prefix", "J", false}}, {},
                                        SigV4Signer::kEmptyPayloadHash);
        const SigningResult r = SigV4Signer::signHeaders(in, s3DocContext(), true);
        QCOMPARE(r.signature,
                 QByteArray("34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"));
    }

    // https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-query-string-auth.html
    void s3DocPresignedGet()
    {
        Credentials c;
        c.accessKeyId = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
        c.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
        PresignRequest req;
        req.bucket = QStringLiteral("examplebucket");
        req.key = QStringLiteral("test.txt");
        req.expiresSeconds = 86400;
        const Result<QUrl> url = Presigner::presign(awsEndpoint(), AddressingStyle::VirtualHosted,
                                                    c, req, s3DocContext().time);
        QVERIFY(url);
        QCOMPARE(url->toEncoded(),
                 QByteArray("https://examplebucket.s3.amazonaws.com/test.txt"
                            "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
                            "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
                            "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host"
                            "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404"));
    }

    void presignValidation()
    {
        Credentials c;
        c.accessKeyId = QStringLiteral("AK");
        c.secretAccessKey = QStringLiteral("SK");
        c.sessionToken = QStringLiteral("tok/en+");
        PresignRequest req;
        req.bucket = QStringLiteral("bkt");
        req.key = QStringLiteral("a b");
        req.versionId = QStringLiteral("v1");
        const QDateTime now = QDateTime::currentDateTimeUtc();

        const Result<QUrl> ok = Presigner::presign(awsEndpoint(), AddressingStyle::Path, c, req, now);
        QVERIFY(ok);
        const QByteArray enc = ok->toEncoded();
        QVERIFY(enc.startsWith("https://s3.amazonaws.com/bkt/a%20b?"));
        QVERIFY(enc.contains("&X-Amz-Security-Token=tok%2Fen%2B&"));
        QVERIFY(enc.contains("&versionId=v1"));

        // "Download as": signed with the URL; GET only.
        req.responseContentDisposition = QStringLiteral("attachment; filename=\"a b.txt\"");
        const Result<QUrl> named = Presigner::presign(awsEndpoint(), AddressingStyle::Path, c, req, now);
        QVERIFY(named);
        QVERIFY(named->toEncoded().contains("response-content-disposition=attachment%3B%20filename%3D%22a%20b.txt%22"));
        req.method = "PUT";
        QCOMPARE(Presigner::presign(awsEndpoint(), AddressingStyle::Path, c, req, now).error().category,
                 ErrorCategory::InvalidRequest);
        req.method = "GET";
        req.responseContentDisposition.clear();

        req.expiresSeconds = Presigner::kMaxExpirySeconds + 1;
        QCOMPARE(Presigner::presign(awsEndpoint(), AddressingStyle::Path, c, req, now).error().category,
                 ErrorCategory::InvalidRequest);
        req.expiresSeconds = 0;
        QVERIFY(!Presigner::presign(awsEndpoint(), AddressingStyle::Path, c, req, now));
        req.expiresSeconds = 60;
        QCOMPARE(Presigner::presign(awsEndpoint(), AddressingStyle::Path, Credentials(), req, now)
                     .error().category,
                 ErrorCategory::Credentials);
    }

    // --- building blocks ----------------------------------------------------
    void canonicalHeaderRules()
    {
        QByteArray names;
        const QByteArray canonical = SigV4Signer::canonicalHeaders(
            {{"X-B", "  two   words "}, {"x-a", "1"}, {"X-A", "2"}, {"x-c", ""}, {"x-c", "z"}},
            &names);
        QCOMPARE(canonical, QByteArray("x-a:1,2\nx-b:two words\nx-c:,z\n"));
        QCOMPARE(names, QByteArray("x-a;x-b;x-c"));
    }

    void sessionTokenIsSignedInHeaderMode()
    {
        SigningContext ctx = s3DocContext();
        ctx.credentials.sessionToken = QStringLiteral("TOKEN");
        const SigningInput in = s3Input("GET", QStringLiteral("k"), {}, {},
                                        SigV4Signer::kEmptyPayloadHash);
        const SigningResult r = SigV4Signer::signHeaders(in, ctx, true);
        QVERIFY(r.canonicalRequest.contains("\nx-amz-security-token:TOKEN\n"));
        QVERIFY(r.canonicalRequest.contains(
            "\nhost;x-amz-content-sha256;x-amz-date;x-amz-security-token\n"));
    }
};

QTEST_GUILESS_MAIN(TestSigV4)
#include "test_sigv4.moc"
