#include <qbucket/s3/testing/FakeS3Server.h>

#include "http/UriEncoding.h"
#include "signing/SigV4Signer.h"

#include <QDateTime>
#include <QHostAddress>
#include <QLocale>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <optional>

namespace qbucket::s3::testing {

// --- RecordedRequest / FakeResponse -------------------------------------------

QByteArray RecordedRequest::header(const QByteArray& name) const
{
    for (const auto& [n, v] : headers) {
        if (n.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    }
    return {};
}

namespace {

QList<QPair<QByteArray, QByteArray>> splitQuery(const QByteArray& query)
{
    QList<QPair<QByteArray, QByteArray>> out;
    if (query.isEmpty())
        return out;
    for (const QByteArray& part : query.split('&')) {
        const qsizetype eq = part.indexOf('=');
        out.append(eq < 0 ? qMakePair(part, QByteArray())
                          : qMakePair(part.left(eq), part.mid(eq + 1)));
    }
    return out;
}

} // namespace

bool RecordedRequest::hasQueryItem(const QByteArray& name) const
{
    const auto items = splitQuery(query);
    return std::any_of(items.begin(), items.end(), [&](const auto& p) { return p.first == name; });
}

QByteArray RecordedRequest::queryItem(const QByteArray& name) const
{
    for (const auto& [n, v] : splitQuery(query)) {
        if (n == name)
            return v.isNull() ? QByteArray("") : v;
    }
    return {};
}

FakeResponse FakeResponse::ok(const QByteArray& body, const QByteArray& contentType)
{
    FakeResponse r;
    r.body = body;
    if (!body.isEmpty())
        r.headers.append({"Content-Type", contentType});
    return r;
}

FakeResponse FakeResponse::s3Error(int status, const QByteArray& code, const QByteArray& message)
{
    FakeResponse r;
    r.status = status;
    r.headers.emplaceBack("Content-Type", "application/xml");
    r.body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>" + code
           + "</Code><Message>" + message + "</Message><RequestId>FAKEREQ</RequestId>"
           + "<HostId>FAKEHOST</HostId></Error>";
    return r;
}

FakeResponse& FakeResponse::withHeader(const QByteArray& name, const QByteArray& value)
{
    headers.append({name, value});
    return *this;
}

// --- Impl --------------------------------------------------------------------

namespace {

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 307: return "Temporary Redirect";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Status";
    }
}

QByteArray httpDate(const QDateTime& utc)
{
    return QLocale::c().toString(utc, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'")).toLatin1();
}

bool hasHeader(const RawHeaders& headers, const QByteArray& name)
{
    return std::any_of(headers.begin(), headers.end(), [&](const auto& h) {
        return h.first.compare(name, Qt::CaseInsensitive) == 0;
    });
}

// SigV4 canonical query from raw received bytes (decode, re-encode, sort).
QByteArray canonicalQueryFromWire(const QByteArray& query, const QByteArray& skipName)
{
    QList<QPair<QByteArray, QByteArray>> pairs;
    for (const auto& [rawName, rawValue] : splitQuery(query)) {
        QByteArray name;
        QByteArray value;
        uriDecode(rawName, false, &name);
        uriDecode(rawValue, false, &value);
        if (name == skipName)
            continue;
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

struct Route {
    Matcher matcher;
    QList<FakeResponse> responses;
    int times = -1;
    int used = 0;
};

bool matches(const Matcher& m, const RecordedRequest& req)
{
    if (!m.method.isEmpty() && m.method != req.method)
        return false;
    if (m.path.isValid() && !m.path.pattern().isEmpty()
        && !m.path.match(QString::fromLatin1(req.path)).hasMatch())
        return false;
    for (const QByteArray& key : m.requiredQueryKeys) {
        if (!req.hasQueryItem(key))
            return false;
    }
    return !m.predicate || m.predicate(req);
}

} // namespace

struct FakeS3Server::Impl {
    FakeS3Server* q = nullptr;
    QTcpServer server;
    QList<Route> routes;
    std::function<FakeResponse(const RecordedRequest&)> fallback;
    QList<RecordedRequest> recorded;
    int connections = 0;
    int requestCounter = 0;
    qint64 clockOffsetSecs = 0;

    bool verify = false;
    Credentials verifyCredentials;
    QByteArray verifyRegion;

    struct Connection {
        QPointer<QTcpSocket> socket;
        QByteArray buffer;
        bool busy = false;
        int index = 0;
    };

    QDateTime now() const { return QDateTime::currentDateTimeUtc().addSecs(clockOffsetSecs); }

    void onNewConnection()
    {
        while (QTcpSocket* socket = server.nextPendingConnection()) {
            auto conn = std::make_shared<Connection>();
            conn->socket = socket;
            conn->index = ++connections;
            QObject::connect(socket, &QTcpSocket::readyRead, q, [this, conn] {
                conn->buffer += conn->socket->readAll();
                process(conn);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void process(const std::shared_ptr<Connection>& conn)
    {
        if (conn->busy || !conn->socket)
            return;
        const qsizetype headerEnd = conn->buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;

        RecordedRequest req;
        req.connection = conn->index;
        const QList<QByteArray> lines = conn->buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() != 3) {
            conn->socket->abort();
            return;
        }
        req.method = requestLine[0];
        req.target = requestLine[1];
        const qsizetype qm = req.target.indexOf('?');
        req.path = qm < 0 ? req.target : req.target.left(qm);
        req.query = qm < 0 ? QByteArray() : req.target.mid(qm + 1);
        qint64 contentLength = 0;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed();
            const QByteArray value = line.mid(colon + 1).trimmed();
            req.headers.append({name, value});
            if (name.compare("content-length", Qt::CaseInsensitive) == 0)
                contentLength = value.toLongLong();
        }
        const qsizetype bodyStart = headerEnd + 4;
        if (conn->buffer.size() - bodyStart < contentLength)
            return;   // wait for the rest of the body
        req.body = conn->buffer.mid(bodyStart, contentLength);
        conn->buffer.remove(0, bodyStart + contentLength);
        conn->busy = true;

        recorded.append(req);
        const int index = int(recorded.size()) - 1;
        Q_EMIT q->requestReceived(index);

        const FakeResponse response = respond(req);
        if (response.delayMs > 0) {
            QTimer::singleShot(response.delayMs, q, [this, conn, req, response] {
                send(conn, req, response);
            });
        } else {
            send(conn, req, response);
        }
    }

    FakeResponse respond(const RecordedRequest& req)
    {
        if (verify) {
            if (std::optional<FakeResponse> rejected = checkSignature(req))
                return *rejected;
        }
        for (Route& route : routes) {
            if (route.times >= 0 && route.used >= route.times)
                continue;
            if (!matches(route.matcher, req))
                continue;
            const qsizetype i = std::min<qsizetype>(route.used, route.responses.size() - 1);
            ++route.used;
            return route.responses.at(i);
        }
        if (fallback)
            return fallback(req);
        qWarning("FakeS3Server: no route for %s %s", req.method.constData(), req.target.constData());
        return FakeResponse::s3Error(418, "FakeS3NoRoute", "no route matched " + req.method + ' ' + req.target);
    }

    std::optional<FakeResponse> checkSignature(const RecordedRequest& req) const
    {
        const auto reject = [](int status, const QByteArray& code, const QByteArray& msg) {
            return std::optional<FakeResponse>(FakeResponse::s3Error(status, code, msg));
        };

        SigningInput in;
        in.method = req.method;
        in.canonicalUri = req.path;
        QByteArray accessKey;
        QByteArray scope;
        QByteArray signedHeaderNames;
        QByteArray signature;
        QByteArray amzDate;
        const bool presigned = req.hasQueryItem("X-Amz-Signature");

        if (presigned) {
            QByteArray credential;
            uriDecode(req.queryItem("X-Amz-Credential"), false, &credential);
            const qsizetype slash = credential.indexOf('/');
            accessKey = credential.left(slash);
            scope = credential.mid(slash + 1);
            uriDecode(req.queryItem("X-Amz-SignedHeaders"), false, &signedHeaderNames);
            signature = req.queryItem("X-Amz-Signature");
            amzDate = req.queryItem("X-Amz-Date");
            in.canonicalQuery = canonicalQueryFromWire(req.query, "X-Amz-Signature");
            in.payloadHash = SigV4Signer::kUnsignedPayload;

            QDateTime signedAt = QDateTime::fromString(QString::fromLatin1(amzDate),
                                                       QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
            signedAt.setTimeZone(QTimeZone::UTC);
            const int expires = req.queryItem("X-Amz-Expires").toInt();
            if (!signedAt.isValid() || signedAt.addSecs(expires) < now())
                return reject(403, "AccessDenied", "Request has expired");
        } else {
            const QByteArray auth = req.header("Authorization");
            if (!auth.startsWith("AWS4-HMAC-SHA256 "))
                return reject(403, "AccessDenied", "missing or unsupported Authorization");
            for (QByteArray part : auth.mid(17).split(',')) {
                part = part.trimmed();
                if (part.startsWith("Credential=")) {
                    const QByteArray c = part.mid(11);
                    accessKey = c.left(c.indexOf('/'));
                    scope = c.mid(c.indexOf('/') + 1);
                } else if (part.startsWith("SignedHeaders=")) {
                    signedHeaderNames = part.mid(14);
                } else if (part.startsWith("Signature=")) {
                    signature = part.mid(10);
                }
            }
            amzDate = req.header("x-amz-date");
            in.canonicalQuery = canonicalQueryFromWire(req.query, {});
            in.payloadHash = req.header("x-amz-content-sha256");
            if (in.payloadHash.isEmpty())
                return reject(400, "InvalidRequest", "missing x-amz-content-sha256");
            if (in.payloadHash != SigV4Signer::kUnsignedPayload
                && in.payloadHash != SigV4Signer::sha256Hex(req.body)) {
                return reject(400, "XAmzContentSHA256Mismatch",
                              "The provided 'x-amz-content-sha256' header does not match what was computed.");
            }
        }

        if (accessKey != verifyCredentials.accessKeyId.toUtf8())
            return reject(403, "InvalidAccessKeyId", "The AWS Access Key Id you provided does not exist in our records.");

        QDateTime time = QDateTime::fromString(QString::fromLatin1(amzDate),
                                               QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
        time.setTimeZone(QTimeZone::UTC);
        if (!time.isValid())
            return reject(403, "AccessDenied", "missing or invalid X-Amz-Date");
        if (!presigned && std::abs(time.secsTo(now())) > 15 * 60) {
            FakeResponse r = FakeResponse::s3Error(
                403, "RequestTimeTooSkewed",
                "The difference between the request time and the current time is too large.");
            r.body.replace("</Error>", "<ServerTime>"
                                           + now().toString(Qt::ISODate).toLatin1()
                                           + "</ServerTime></Error>");
            return r;
        }

        const QList<QByteArray> scopeParts = scope.split('/');
        if (scopeParts.size() != 4 || scopeParts[1] != verifyRegion) {
            FakeResponse r = FakeResponse::s3Error(
                400, "AuthorizationHeaderMalformed",
                "the region in the credential scope is wrong; expecting '" + verifyRegion + "'");
            r.body.replace("</Error>", "<Region>" + verifyRegion + "</Region></Error>");
            // Like S3: HEAD responses carry no body, so the hint is a header too.
            r.withHeader("x-amz-bucket-region", verifyRegion);
            return r;
        }

        for (const QByteArray& name : signedHeaderNames.split(';')) {
            bool found = false;
            for (const auto& [n, v] : req.headers) {
                if (n.compare(name, Qt::CaseInsensitive) == 0) {
                    in.headers.append({n, v});
                    found = true;
                }
            }
            if (!found)
                return reject(403, "AccessDenied", "signed header missing from request: " + name);
        }

        SigningContext ctx;
        ctx.credentials = verifyCredentials;
        ctx.region = scopeParts[1];
        ctx.service = scopeParts[2];
        ctx.time = time;
        const QByteArray canonical = SigV4Signer::canonicalRequest(in);
        if (SigV4Signer::signature(canonical, ctx) != signature) {
            return reject(403, "SignatureDoesNotMatch",
                          "The request signature we calculated does not match the signature you "
                          "provided. Canonical request: " + canonical.toPercentEncoding());
        }
        return std::nullopt;
    }

    void send(const std::shared_ptr<Connection>& conn, const RecordedRequest& req,
              const FakeResponse& response)
    {
        QTcpSocket* socket = conn->socket;
        if (!socket || socket->state() != QAbstractSocket::ConnectedState)
            return;
        if (response.dropWithoutResponse) {
            socket->abort();
            return;
        }

        const bool head = req.method == "HEAD";
        QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + ' '
                       + reasonPhrase(response.status) + "\r\n";
        RawHeaders headers = response.headers;
        if (!hasHeader(headers, "Date"))
            headers.append({"Date", httpDate(now())});
        if (!hasHeader(headers, "x-amz-request-id"))
            headers.append({"x-amz-request-id", "FAKE" + QByteArray::number(++requestCounter)});
        if (!hasHeader(headers, "Content-Length")) {
            const qint64 length = response.declaredContentLength >= 0
                ? response.declaredContentLength
                : response.body.size();
            headers.append({"Content-Length", QByteArray::number(length)});
        }
        if (response.closeConnection)
            headers.emplaceBack("Connection", "close");
        for (const auto& [n, v] : headers)
            out += n + ": " + v + "\r\n";
        out += "\r\n";

        QByteArray body = head ? QByteArray() : response.body;
        if (response.stallAfterBodyBytes >= 0) {
            socket->write(out + body.left(response.stallAfterBodyBytes));
            return;   // stays busy: never answers anything else on this connection
        }
        if (response.closeAfterBodyBytes >= 0) {
            socket->write(out + body.left(response.closeAfterBodyBytes));
            socket->disconnectFromHost();
            return;
        }
        socket->write(out + body);
        if (response.closeConnection) {
            socket->disconnectFromHost();
            return;
        }
        conn->busy = false;
        process(conn);   // a further request may already be buffered
    }
};

// --- FakeS3Server ------------------------------------------------------------

FakeS3Server::FakeS3Server(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->q = this;
    connect(&d->server, &QTcpServer::newConnection, this, [this] { d->onNewConnection(); });
}

FakeS3Server::~FakeS3Server() = default;

bool FakeS3Server::listen()
{
    return d->server.listen(QHostAddress::LocalHost, 0);
}

quint16 FakeS3Server::port() const
{
    return d->server.serverPort();
}

QUrl FakeS3Server::endpointUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port()));
}

void FakeS3Server::addRoute(const Matcher& matcher, const QList<FakeResponse>& responses, int times)
{
    Q_ASSERT(!responses.isEmpty());
    d->routes.append({matcher, responses, times, 0});
}

void FakeS3Server::on(const QByteArray& method, const QString& pathPattern,
                      const FakeResponse& response, int times)
{
    Matcher m;
    m.method = method;
    m.path = QRegularExpression(pathPattern);
    addRoute(m, {response}, times);
}

void FakeS3Server::setFallbackHandler(std::function<FakeResponse(const RecordedRequest&)> handler)
{
    d->fallback = std::move(handler);
}

void FakeS3Server::clearRoutes()
{
    d->routes.clear();
}

void FakeS3Server::setVerifySignatures(const Credentials& credentials, const QByteArray& region)
{
    d->verify = true;
    d->verifyCredentials = credentials;
    d->verifyRegion = region;
}

void FakeS3Server::setClockOffsetSecs(qint64 offset)
{
    d->clockOffsetSecs = offset;
}

QList<RecordedRequest> FakeS3Server::requests() const
{
    return d->recorded;
}

int FakeS3Server::requestCount() const
{
    return int(d->recorded.size());
}

int FakeS3Server::connectionCount() const
{
    return d->connections;
}

void FakeS3Server::clearRequests()
{
    d->recorded.clear();
}

} // namespace qbucket::s3::testing
