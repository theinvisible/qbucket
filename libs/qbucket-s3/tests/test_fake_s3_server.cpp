#include "http/S3Uri.h"
#include "signing/SigV4Signer.h"

#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

struct Reply {
    int status = 0;
    QByteArray body;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
};

Reply send(QNetworkAccessManager& nam, QNetworkRequest req, const QByteArray& method = "GET",
           const QByteArray& body = {})
{
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply* reply = nam.sendCustomRequest(req, method, body);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!reply->isFinished())
        finished.wait(10000);
    Reply r;
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->isOpen())
        r.body = reply->readAll();
    r.error = reply->error();
    reply->deleteLater();
    return r;
}

QNetworkRequest at(const FakeS3Server& server, const QByteArray& target)
{
    return QNetworkRequest(QUrl::fromEncoded(server.endpointUrl().toEncoded() + target));
}

Credentials creds()
{
    Credentials c;
    c.accessKeyId = QStringLiteral("AKIDFAKE");
    c.secretAccessKey = QStringLiteral("fake/secret+key");
    return c;
}

// Builds and signs a request exactly the way the client will.
QNetworkRequest signedRequest(const FakeS3Server& server, const QByteArray& method,
                              const QString& key, const QByteArray& body, const Credentials& c,
                              const QByteArray& region, const QDateTime& time,
                              const QueryParams& query = {})
{
    Endpoint ep;
    ep.url = server.endpointUrl();
    ep.region = QString::fromLatin1(region);
    const Result<S3Uri> uri = S3Uri::build(ep, AddressingStyle::Path, QStringLiteral("bkt"), key, query);
    Q_ASSERT(uri);
    SigningInput in;
    in.method = method;
    in.canonicalUri = uri->canonicalUri();
    in.canonicalQuery = uri->canonicalQuery();
    in.headers = {{"host", uri->host()}};
    in.payloadHash = SigV4Signer::sha256Hex(body);
    SigningContext ctx;
    ctx.credentials = c;
    ctx.region = region;
    ctx.time = time;
    QNetworkRequest req(uri->url());
    for (const auto& [n, v] : SigV4Signer::signHeaders(in, ctx, true).addedHeaders)
        req.setRawHeader(n, v);
    return req;
}

} // namespace

class TestFakeS3Server : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void routesAndRecording()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("GET", QStringLiteral("^/bkt/"), FakeResponse::ok("hello", "text/plain"));
        QNetworkAccessManager nam;

        const Reply r = send(nam, at(server, "/bkt/a%20b%2B?versioning&x=%2F"));
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body, QByteArray("hello"));

        QCOMPARE(server.requestCount(), 1);
        const RecordedRequest rec = server.requests().first();
        QCOMPARE(rec.method, QByteArray("GET"));
        QCOMPARE(rec.target, QByteArray("/bkt/a%20b%2B?versioning&x=%2F"));
        QCOMPARE(rec.path, QByteArray("/bkt/a%20b%2B"));
        QVERIFY(rec.hasQueryItem("versioning"));
        QCOMPARE(rec.queryItem("versioning"), QByteArray(""));
        QCOMPARE(rec.queryItem("x"), QByteArray("%2F"));
        QVERIFY(rec.queryItem("missing").isNull());
        QCOMPARE(rec.header("HOST"), QByteArray("127.0.0.1:") + QByteArray::number(server.port()));
    }

    void bodiesAreRecorded()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("PUT", QString(), FakeResponse::ok());
        QNetworkAccessManager nam;
        const QByteArray payload(70000, 'x');
        QCOMPARE(send(nam, at(server, "/bkt/k"), "PUT", payload).status, 200);
        QCOMPARE(server.requests().first().body, payload);
    }

    void sequencesTimesAndFallback()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        Matcher m;
        m.method = "GET";
        m.requiredQueryKeys = {"list-type"};
        server.addRoute(m, {FakeResponse::s3Error(503, "SlowDown"), FakeResponse::ok("<a/>")});
        server.on("GET", QStringLiteral("^/once$"), FakeResponse::ok("first"), 1);
        server.on("GET", QStringLiteral("^/once$"), FakeResponse::ok("later"));
        server.setFallbackHandler([](const RecordedRequest& r) {
            return FakeResponse::ok("fallback:" + r.path);
        });
        QNetworkAccessManager nam;

        QCOMPARE(send(nam, at(server, "/b?list-type=2")).status, 503);
        QCOMPARE(send(nam, at(server, "/b?list-type=2")).status, 200);
        QCOMPARE(send(nam, at(server, "/b?list-type=2")).body, QByteArray("<a/>"));   // last repeats
        QCOMPARE(send(nam, at(server, "/once")).body, QByteArray("first"));
        QCOMPARE(send(nam, at(server, "/once")).body, QByteArray("later"));
        QCOMPARE(send(nam, at(server, "/other")).body, QByteArray("fallback:/other"));
    }

    void unmatchedRequestsAreLoud()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        QNetworkAccessManager nam;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no route for GET /nope")));
        const Reply r = send(nam, at(server, "/nope"));
        QCOMPARE(r.status, 418);
        QVERIFY(r.body.contains("FakeS3NoRoute"));
    }

    void keepAliveAndConnectionClose()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("GET", QStringLiteral("^/keep$"), FakeResponse::ok("k"));
        FakeResponse closing = FakeResponse::ok("c");
        closing.closeConnection = true;
        server.on("GET", QStringLiteral("^/close$"), closing);
        QNetworkAccessManager nam;

        QCOMPARE(send(nam, at(server, "/keep")).status, 200);
        QCOMPARE(send(nam, at(server, "/keep")).status, 200);
        QCOMPARE(server.connectionCount(), 1);
        QCOMPARE(send(nam, at(server, "/close")).status, 200);
        QCOMPARE(send(nam, at(server, "/keep")).status, 200);
        QCOMPARE(server.connectionCount(), 2);
        QCOMPARE(server.requests().last().connection, 2);
    }

    void delayedResponse()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        FakeResponse slow = FakeResponse::ok("late");
        slow.delayMs = 200;
        server.on("GET", QString(), slow);
        QNetworkAccessManager nam;
        QElapsedTimer t;
        t.start();
        QCOMPARE(send(nam, at(server, "/x")).body, QByteArray("late"));
        QVERIFY(t.elapsed() >= 180);
    }

    void truncatedBody()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        FakeResponse cut = FakeResponse::ok(QByteArray(100, 'b'));
        cut.closeAfterBodyBytes = 5;
        server.on("GET", QString(), cut);
        QNetworkAccessManager nam;
        const Reply r = send(nam, at(server, "/x"));
        QVERIFY(r.error != QNetworkReply::NoError);
        QVERIFY(r.body.size() <= 5);
    }

    void wrongContentLength()
    {
        FakeResponse lie = FakeResponse::ok(QByteArray(10, 'b'));
        lie.declaredContentLength = 50;
        lie.closeConnection = true;
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("GET", QString(), lie);
        QNetworkAccessManager nam;
        QVERIFY(send(nam, at(server, "/x")).error != QNetworkReply::NoError);
    }

    void stalledResponse()
    {
        FakeResponse stall = FakeResponse::ok(QByteArray(100, 'b'));
        stall.stallAfterBodyBytes = 10;
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("GET", QString(), stall);
        QNetworkAccessManager nam;
        QNetworkRequest req = at(server, "/x");
        req.setTransferTimeout(300);
        QElapsedTimer t;
        t.start();
        const Reply r = send(nam, req);
        QVERIFY(r.error != QNetworkReply::NoError);
        QVERIFY(t.elapsed() < 5000);
    }

    void droppedWithoutResponse()
    {
        FakeResponse drop;
        drop.dropWithoutResponse = true;
        FakeS3Server server;
        QVERIFY(server.listen());
        server.on("DELETE", QString(), drop);
        QNetworkAccessManager nam;
        const Reply r = send(nam, at(server, "/bkt/k"), "DELETE");
        QVERIFY(r.error != QNetworkReply::NoError);
        QCOMPARE(r.status, 0);
        QCOMPARE(server.requestCount(), 1);   // the request did arrive
    }

    // --- signature verification ------------------------------------------
    void acceptsCorrectSignatures()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(creds(), "test-1");
        server.on("", QString(), FakeResponse::ok("<ok/>"));
        QNetworkAccessManager nam;
        const QDateTime now = QDateTime::currentDateTimeUtc();

        const QString nastyKey = QStringLiteral("dir/a b+c%d/é!$&'()*,;=:@");
        Reply r = send(nam, signedRequest(server, "GET", nastyKey, {}, creds(), "test-1", now,
                                          {{"versioning", {}, true}, {"x", "a/b c", false}}));
        QVERIFY2(r.status == 200, r.body.constData());

        const QByteArray body = "payload";
        r = send(nam, signedRequest(server, "PUT", QStringLiteral("k"), body, creds(), "test-1", now),
                 "PUT", body);
        QVERIFY2(r.status == 200, r.body.constData());
    }

    void rejectsBadSignatures()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(creds(), "test-1");
        server.on("", QString(), FakeResponse::ok("<ok/>"));
        QNetworkAccessManager nam;
        const QDateTime now = QDateTime::currentDateTimeUtc();

        Credentials wrongSecret = creds();
        wrongSecret.secretAccessKey = QStringLiteral("other");
        Reply r = send(nam, signedRequest(server, "GET", QStringLiteral("k"), {}, wrongSecret, "test-1", now));
        QCOMPARE(r.status, 403);
        QVERIFY(r.body.contains("SignatureDoesNotMatch"));

        Credentials wrongKey = creds();
        wrongKey.accessKeyId = QStringLiteral("AKIDOTHER");
        r = send(nam, signedRequest(server, "GET", QStringLiteral("k"), {}, wrongKey, "test-1", now));
        QVERIFY(r.body.contains("InvalidAccessKeyId"));

        r = send(nam, signedRequest(server, "GET", QStringLiteral("k"), {}, creds(), "eu-other", now));
        QCOMPARE(r.status, 400);
        QVERIFY(r.body.contains("<Region>test-1</Region>"));

        // Body differs from the signed payload hash.
        r = send(nam, signedRequest(server, "PUT", QStringLiteral("k"), "signed", creds(), "test-1", now),
                 "PUT", "sent");
        QVERIFY(r.body.contains("XAmzContentSHA256Mismatch"));

        // Unsigned request.
        r = send(nam, at(server, "/bkt/k"));
        QCOMPARE(r.status, 403);
    }

    void clockSkew()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(creds(), "test-1");
        server.setClockOffsetSecs(20 * 60);
        server.on("", QString(), FakeResponse::ok("<ok/>"));
        QNetworkAccessManager nam;

        Reply r = send(nam, signedRequest(server, "GET", QStringLiteral("k"), {}, creds(), "test-1",
                                          QDateTime::currentDateTimeUtc()));
        QCOMPARE(r.status, 403);
        QVERIFY(r.body.contains("RequestTimeTooSkewed"));
        QVERIFY(r.body.contains("<ServerTime>"));

        r = send(nam, signedRequest(server, "GET", QStringLiteral("k"), {}, creds(), "test-1",
                                    QDateTime::currentDateTimeUtc().addSecs(20 * 60)));
        QCOMPARE(r.status, 200);
    }

    void presignedUrls()
    {
        FakeS3Server server;
        QVERIFY(server.listen());
        server.setVerifySignatures(creds(), "test-1");
        server.on("GET", QString(), FakeResponse::ok("data", "application/octet-stream"));
        QNetworkAccessManager nam;
        Endpoint ep;
        ep.url = server.endpointUrl();
        ep.region = QStringLiteral("test-1");
        PresignRequest pr;
        pr.bucket = QStringLiteral("bkt");
        pr.key = QStringLiteral("dir/a b.txt");
        pr.expiresSeconds = 60;

        Result<QUrl> url = Presigner::presign(ep, AddressingStyle::Path, creds(), pr,
                                              QDateTime::currentDateTimeUtc());
        QVERIFY(url);
        QCOMPARE(send(nam, QNetworkRequest(*url)).body, QByteArray("data"));

        url = Presigner::presign(ep, AddressingStyle::Path, creds(), pr,
                                 QDateTime::currentDateTimeUtc().addSecs(-3600));
        const Reply expired = send(nam, QNetworkRequest(*url));
        QCOMPARE(expired.status, 403);
        QVERIFY(expired.body.contains("expired"));
    }
};

QTEST_GUILESS_MAIN(TestFakeS3Server)
#include "test_fake_s3_server.moc"
