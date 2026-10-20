#include "logging/Redaction.h"
#include "signing/SigV4Signer.h"

#include <qbucket/s3/Presigner.h>

#include <QtTest>

using namespace qbucket::s3;

namespace {

const QString kAccessKey = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
const QString kSecret = QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
const QString kToken = QStringLiteral("FQoGZXIvYXdzEJr//////////wEaDOkSessionTokenSecret==");

} // namespace

class TestRedaction : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void shortensAccessKeys()
    {
        QCOMPARE(redact::shortenAccessKeyId(kAccessKey), QStringLiteral("AKIA…MPLE"));
        QCOMPARE(redact::shortenAccessKeyId(QStringLiteral("short")), QStringLiteral("sh…"));
    }

    void authorizationHeader()
    {
        // A real signature from the signer, so the format can't drift.
        SigningInput in;
        in.method = "GET";
        in.canonicalUri = "/k";
        in.headers = {{"host", "s3.example.com"}};
        in.payloadHash = SigV4Signer::kEmptyPayloadHash;
        SigningContext ctx;
        ctx.credentials = {kAccessKey, kSecret, kToken, {}};
        ctx.region = "fsn1";
        ctx.time = QDateTime::currentDateTimeUtc();
        const SigningResult r = SigV4Signer::signHeaders(in, ctx, true);

        for (const auto& [name, value] : r.addedHeaders) {
            const QByteArray safe = redact::headerValue(name, value);
            QVERIFY2(!safe.contains(r.signature), name.constData());
            QVERIFY2(!safe.contains(kToken.toUtf8()), name.constData());
            QVERIFY2(!safe.contains(kAccessKey.toUtf8()), name.constData());
            QVERIFY2(!safe.contains(kSecret.toUtf8()), name.constData());
            if (name == "Authorization") {
                QVERIFY(safe.startsWith("AWS4-HMAC-SHA256 Credential=AKIA"));
                QVERIFY(safe.contains("/fsn1/s3/aws4_request"));
                QVERIFY(safe.endsWith("Signature=<redacted>"));
            }
        }
    }

    void secretHeaders()
    {
        QCOMPARE(redact::headerValue("X-Amz-Security-Token", "tok"), QByteArray("<redacted>"));
        QCOMPARE(redact::headerValue("x-amz-server-side-encryption-customer-key", "k"),
                 QByteArray("<redacted>"));
        QCOMPARE(redact::headerValue("x-amz-copy-source-server-side-encryption-customer-key", "k"),
                 QByteArray("<redacted>"));
        QCOMPARE(redact::headerValue("x-amz-server-side-encryption-customer-key-MD5", "md5"),
                 QByteArray("md5"));
        QCOMPARE(redact::headerValue("Content-Type", "text/plain"), QByteArray("text/plain"));
    }

    void presignedUrl()
    {
        Endpoint ep;
        ep.url = QUrl(QStringLiteral("https://fsn1.your-objectstorage.com"));
        ep.region = QStringLiteral("fsn1");
        PresignRequest req;
        req.bucket = QStringLiteral("bkt");
        req.key = QStringLiteral("dir/file name.txt");
        const Result<QUrl> url = Presigner::presign(ep, AddressingStyle::Path,
                                                    {kAccessKey, kSecret, kToken, {}}, req,
                                                    QDateTime::currentDateTimeUtc());
        QVERIFY(url);
        const QString sig = QUrlQuery(*url).queryItemValue(QStringLiteral("X-Amz-Signature"));
        QVERIFY(!sig.isEmpty());

        const QString safe = redact::url(*url);
        QVERIFY(!safe.contains(sig));
        QVERIFY(!safe.contains(kAccessKey));
        QVERIFY(!safe.contains(QStringLiteral("SessionTokenSecret")));
        QVERIFY(safe.contains(QStringLiteral("X-Amz-Signature=<redacted>")));
        QVERIFY(safe.contains(QStringLiteral("X-Amz-Credential=AKIA…MPLE")));
        QVERIFY(safe.contains(QStringLiteral("/bkt/dir/file")));

        const QUrl plain(QStringLiteral("https://h/b/k?versionId=1"));
        QCOMPARE(redact::url(plain), plain.toDisplayString());
    }
};

QTEST_GUILESS_MAIN(TestRedaction)
#include "test_redaction.moc"
