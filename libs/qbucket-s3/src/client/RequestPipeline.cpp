#include "client/RequestPipeline.h"

#include "core/ErrorMapping.h"
#include "core/HttpDate.h"
#include "core/Mailbox.h"
#include "logging/Redaction.h"
#include "xml/XmlParsers.h"

#include <qbucket/s3/Logging.h>

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

#if QT_CONFIG(ssl)
#include <QSslCertificate>
#include <QSslError>
#endif

namespace qbucket::s3 {

QByteArray RawResponse::header(const QByteArray& name) const
{
    for (const auto& [n, v] : headers) {
        if (n.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    }
    return {};
}

struct RequestPipeline::Call {
    quint64 id = 0;
    RequestSpec spec;
    Done done;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> retryTimer;
    QString redactedUrl;
    QDateTime startedAt;
    QElapsedTimer timer;
    int attempt = 1;
    int transientFailures = 0;
    bool aborted = false;
    bool tooLarge = false;
    bool finished = false;
    bool skewRetried = false;
    bool redirected = false;
    bool credentialsRetried = false;
};

namespace {

ErrorCategory categoryForNetworkError(QNetworkReply::NetworkError e)
{
    switch (e) {
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::InsecureRedirectError:
        return ErrorCategory::Tls;
    case QNetworkReply::ProtocolFailure:
    case QNetworkReply::ProtocolInvalidOperationError:
    case QNetworkReply::ProtocolUnknownError:
        return ErrorCategory::Protocol;
    default:
        return ErrorCategory::Transient;
    }
}

// Network errors where the request provably never reached the server.
bool neverSent(QNetworkReply::NetworkError e)
{
    return e == QNetworkReply::ConnectionRefusedError || e == QNetworkReply::HostNotFoundError
        || e == QNetworkReply::SslHandshakeFailedError
        || e == QNetworkReply::ProxyConnectionRefusedError
        || e == QNetworkReply::ProxyNotFoundError;
}

} // namespace

RequestPipeline::RequestPipeline(ClientConfig config, QObject* parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_retry(m_config.retry)
    , m_mailbox(Mailbox::create())
{
    Q_ASSERT(m_config.profile);
    Q_ASSERT(m_config.credentials);
    if (!m_config.clockOffsetSecs)
        m_config.clockOffsetSecs = std::make_shared<std::atomic<qint64>>(0);
}

QDateTime RequestPipeline::correctedNow() const
{
    return QDateTime::currentDateTimeUtc().addSecs(m_config.clockOffsetSecs->load());
}

Endpoint RequestPipeline::endpointFor(const QString& bucket) const
{
    Endpoint endpoint = m_config.endpoint;
    const auto it = m_bucketRegions.constFind(bucket);
    if (!bucket.isEmpty() && it != m_bucketRegions.constEnd()) {
        endpoint.region = *it;
        const QUrl regional = m_config.profile->endpointFor(*it);
        if (regional.isValid())
            endpoint.url = regional;
    }
    return endpoint;
}

RequestPipeline::~RequestPipeline()
{
    // Destruction implies cancellation; callbacks are not invoked any more.
    for (auto& [id, call] : m_calls) {
        call->finished = true;
        if (call->reply)
            call->reply->abort();
    }
}

QNetworkAccessManager* RequestPipeline::nam()
{
    // Created lazily, so it is born in the thread the pipeline runs in.
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        m_nam->setAutoDeleteReplies(false);
        m_nam->setCookieJar(nullptr);
    }
    return m_nam;
}

quint64 RequestPipeline::execute(RequestSpec spec, Done done)
{
    auto call = std::make_shared<Call>();
    call->id = m_nextId++;
    call->spec = std::move(spec);
    call->done = std::move(done);
    m_calls.emplace(call->id, call);

    // Always asynchronous, also when credentials are cached.
    QTimer::singleShot(0, this, [this, call] { attempt(call); });
    return call->id;
}

void RequestPipeline::attempt(const std::shared_ptr<Call>& call)
{
    if (call->finished)
        return;
    withCredentials([this, call](Result<Credentials> creds) {
        if (call->finished)
            return;
        if (!creds) {
            finish(call, creds.error());
            return;
        }
        start(call, creds.value());
    });
}

void RequestPipeline::withCredentials(std::function<void(Result<Credentials>)> next)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_credentials && !m_credentials->isExpired(now)) {
        next(*m_credentials);
        return;
    }
    m_credentialWaiters.push_back(std::move(next));
    if (m_fetchingCredentials)
        return;
    m_fetchingCredentials = true;

    // The provider may answer from any thread; continue on ours.
    const std::weak_ptr<Mailbox> box = m_mailbox;
    const QPointer<RequestPipeline> self = this;
    m_config.credentials->fetch([box, self](Result<Credentials> result) {
        Mailbox::post(box, [self, result = std::move(result)]() mutable {
            if (!self)
                return;
            self->m_fetchingCredentials = false;
            if (result) {
                self->m_credentials = result.value();
            } else {
                Error e = std::move(result).error();
                e.category = ErrorCategory::Credentials;
                result = e;
            }
            const auto waiters = std::move(self->m_credentialWaiters);
            self->m_credentialWaiters.clear();
            for (const auto& w : waiters)
                w(result);
        });
    });
}

void RequestPipeline::start(const std::shared_ptr<Call>& call, const Credentials& credentials)
{
    const RequestSpec& spec = call->spec;
    const ProviderProfile& profile = *m_config.profile;
    const Endpoint endpoint = endpointFor(spec.bucket);

    RequestContext ctx;
    ctx.operation = spec.operation;
    ctx.method = spec.method;
    ctx.bucket = spec.bucket;
    ctx.key = spec.key;
    ctx.headers = spec.headers;
    ctx.addressing = endpoint.addressing == AddressingStyle::Auto ? profile.defaultAddressing()
                                                                   : endpoint.addressing;
    ctx.https = endpoint.isHttps();
    profile.prepareRequest(ctx);

    const Result<S3Uri> uri = S3Uri::build(endpoint, ctx.addressing, spec.bucket, spec.key, spec.query);
    if (!uri) {
        finish(call, uri.error());
        return;
    }

    SigningInput in;
    in.method = spec.method;
    in.canonicalUri = uri->canonicalUri();
    in.canonicalQuery = uri->canonicalQuery();
    in.headers = HeaderList{{"host", uri->host()}};
    for (const auto& h : ctx.headers)
        in.headers.append(h);
    if (!spec.body.isEmpty() || !spec.contentType.isEmpty())
        in.headers.append({"Content-Type", spec.contentType});
    if (spec.body.isEmpty())
        in.payloadHash = SigV4Signer::kEmptyPayloadHash;
    else if (ctx.unsignedPayload && ctx.https)
        in.payloadHash = SigV4Signer::kUnsignedPayload;
    else
        in.payloadHash = SigV4Signer::sha256Hex(spec.body);

    SigningContext sctx;
    sctx.credentials = credentials;
    sctx.region = profile.signingRegion(endpoint.region).toUtf8();
    sctx.time = correctedNow();
    const SigningResult signedResult = SigV4Signer::signHeaders(in, sctx, true);

    QNetworkRequest req(uri->url());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setTransferTimeout(m_config.transferTimeoutMs);
    for (const auto& [name, value] : in.headers) {
        if (name != "host")   // QNAM derives Host from the URL — identical by construction
            req.setRawHeader(name, value);
    }
    for (const auto& [name, value] : signedResult.addedHeaders)
        req.setRawHeader(name, value);
    req.setRawHeader("User-Agent", m_config.userAgent);
    // Explicit, so QNAM never decompresses (a gzip-encoded object must
    // arrive byte for byte) and the server doesn't compress listings.
    req.setRawHeader("Accept-Encoding", "identity");

    call->redactedUrl = redact::url(uri->url());
    call->startedAt = QDateTime::currentDateTimeUtc();
    call->timer.start();

    QNetworkReply* reply = spec.method == "HEAD"
        ? nam()->head(req)
        : nam()->sendCustomRequest(req, spec.method, spec.body);
    call->reply = reply;

    connect(reply, &QNetworkReply::downloadProgress, this, [this, call](qint64 received, qint64) {
        if (received > m_config.maxResponseBytes && !call->tooLarge && call->reply) {
            call->tooLarge = true;
            call->reply->abort();
        }
    });
#if QT_CONFIG(ssl)
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError>& errors) {
        // Per-account certificate pinning: accept exactly the pinned
        // certificate, never a global "ignore errors".
        const QByteArray digest =
            reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256);
        if (!digest.isEmpty() && m_config.endpoint.pinnedCertificateSha256.contains(digest))
            reply->ignoreSslErrors(errors);
    });
#endif
    connect(reply, &QNetworkReply::finished, this, [this, call] { onFinished(call); });

    qCDebug(lcHttp).noquote() << spec.method << call->redactedUrl;
}

void RequestPipeline::onFinished(const std::shared_ptr<Call>& call)
{
    QNetworkReply* reply = call->reply;
    if (!reply || call->finished)
        return;
    reply->deleteLater();
    call->reply = nullptr;

    const RequestSpec& spec = call->spec;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();

    RawResponse raw;
    raw.status = status;
    raw.headers = reply->rawHeaderPairs();
    if (reply->isOpen())
        raw.body = reply->readAll();
    const QByteArray requestId = raw.header("x-amz-request-id");

    const auto withContext = [&](Error e) {
        if (e.resource.isEmpty())
            e.resource = spec.key.isEmpty() ? spec.bucket : spec.bucket + u'/' + spec.key;
        if (e.requestId.isEmpty())
            e.requestId = QString::fromLatin1(requestId);
        return e;
    };

    Result<RawResponse> result = raw;
    if (call->aborted) {
        result = withContext(Error::make(ErrorCategory::Cancelled, QStringLiteral("request cancelled")));
    } else if (call->tooLarge) {
        result = withContext(Error::make(
            ErrorCategory::InvalidRequest,
            QStringLiteral("response exceeds the in-memory limit of %1 bytes").arg(m_config.maxResponseBytes)));
    } else if (status == 0) {
        Error e = Error::make(categoryForNetworkError(netError), reply->errorString());
        e.networkError = int(netError);
        e.outcomeUnknown = !spec.idempotent && !neverSent(netError);
        result = withContext(e);
    } else if (status >= 200 && status < 300 && netError != QNetworkReply::NoError) {
        // Headers arrived, the body didn't (connection dropped, length mismatch).
        Error e = Error::make(ErrorCategory::Transient,
                              QStringLiteral("connection lost while reading the response: %1")
                                  .arg(reply->errorString()));
        e.httpStatus = status;
        e.networkError = int(netError);
        e.outcomeUnknown = !spec.idempotent;
        result = withContext(e);
    } else if (status >= 300
               || (spec.mayReturn200Error && xml::rootElementName(raw.body) == QLatin1String("Error"))) {
        Error e = errorFromResponse(status, raw.body, QString::fromLatin1(requestId),
                                    QString::fromLatin1(raw.header("x-amz-id-2")));
        e = withContext(e);
        m_config.profile->mapError(e);
        result = e;
    }

    if (!result && result.error().category == ErrorCategory::Authentication) {
        // Rejected credentials: reload them for the next request.
        m_credentials.reset();
        m_config.credentials->invalidate();
    }

    // Keep presigned URLs right even when nothing failed.
    if (status != 0 && result)
        learnClockOffset(parseHttpDate(raw.header("date")));

    notifyObserver(*call, result, status, raw.body, requestId);
    if (!result && !call->aborted && handleFailure(call, raw, result.error()))
        return;
    finish(call, std::move(result));
}

void RequestPipeline::learnClockOffset(const QDateTime& serverTime)
{
    if (!serverTime.isValid())
        return;
    // Date has one-second resolution plus network delay: only real skew counts.
    const qint64 offset = QDateTime::currentDateTimeUtc().secsTo(serverTime);
    const qint64 effective = std::abs(offset) <= 60 ? 0 : offset;
    if (std::abs(effective - m_config.clockOffsetSecs->load()) <= 60)
        return;
    m_config.clockOffsetSecs->store(effective);
    qCWarning(lcSigning) << "clock offset to the server is now" << effective << "s";
    Q_EMIT clockSkewDetected(effective);
}

bool RequestPipeline::handleFailure(const std::shared_ptr<Call>& call, const RawResponse& raw,
                                    const Error& error)
{
    const RequestSpec& spec = call->spec;

    // 1. Clock skew: correct from the server's time and re-sign once. The
    //    server rejected the request before executing it — safe for every
    //    operation.
    if (error.category == ErrorCategory::ClockSkew && !call->skewRetried) {
        QDateTime serverTime;
        if (const auto body = xml::parseErrorBody(raw.body))
            serverTime = body->serverTime;
        if (!serverTime.isValid())
            serverTime = parseHttpDate(raw.header("date"));
        if (serverTime.isValid()) {
            const qint64 offset = QDateTime::currentDateTimeUtc().secsTo(serverTime);
            m_config.clockOffsetSecs->store(offset);
            qCWarning(lcSigning) << "RequestTimeTooSkewed: correcting signing time by" << offset << "s";
            Q_EMIT clockSkewDetected(offset);
            call->skewRetried = true;
            retryLater(call, 0, QStringLiteral("clock skew corrected"));
            return true;
        }
    }

    // 2. Wrong region: learn the bucket's region and re-route once. Never
    //    follow a Location header to an arbitrary host.
    // HEAD responses have no body: then the region header alone tells.
    const bool regionHint = error.category == ErrorCategory::Redirect
        || ((error.httpStatus == 301 || error.httpStatus == 307 || error.httpStatus == 400)
            && !raw.header("x-amz-bucket-region").isEmpty());
    if (regionHint && !call->redirected && !spec.bucket.isEmpty()) {
        QString region = QString::fromLatin1(raw.header("x-amz-bucket-region"));
        if (region.isEmpty()) {
            if (const auto body = xml::parseErrorBody(raw.body))
                region = body->region;
        }
        if (!region.isEmpty() && region != endpointFor(spec.bucket).region) {
            m_bucketRegions.insert(spec.bucket, region);
            call->redirected = true;
            retryLater(call, 0, QStringLiteral("bucket is in region ") + region);
            return true;
        }
    }

    // 3. Expired temporary credentials: reload (already invalidated) once.
    if (error.category == ErrorCategory::Authentication && !call->credentialsRetried
        && (error.code == QLatin1String("ExpiredToken")
            || error.code == QLatin1String("TokenRefreshRequired"))) {
        call->credentialsRetried = true;
        retryLater(call, 0, QStringLiteral("credentials expired"));
        return true;
    }

    // 4. Transient failures, idempotent-only (RetryPolicy).
    std::optional<int> retryAfter;
    bool ok = false;
    const int seconds = raw.header("retry-after").trimmed().toInt(&ok);
    if (ok)
        retryAfter = seconds;
    if (const auto delay = m_retry.nextDelayMs(++call->transientFailures, error, spec.idempotent, retryAfter)) {
        retryLater(call, *delay, error.toString());
        return true;
    }
    return false;
}

void RequestPipeline::retryLater(const std::shared_ptr<Call>& call, int delayMs, const QString& why)
{
    ++call->attempt;
    qCInfo(lcRetry).noquote() << operationName(call->spec.operation) << "attempt" << call->attempt
                              << "in" << delayMs << "ms:" << why;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, call, timer] {
        timer->deleteLater();
        call->retryTimer = nullptr;
        attempt(call);
    });
    call->retryTimer = timer;
    timer->start(delayMs);
}

void RequestPipeline::notifyObserver(const Call& call, const Result<RawResponse>& result, int status,
                                     const QByteArray& body, const QByteArray& requestId) const
{
    const qint64 ms = call.timer.isValid() ? call.timer.elapsed() : 0;
    if (result)
        qCDebug(lcHttp).noquote() << status << call.spec.method << call.redactedUrl << ms << "ms";
    else
        qCInfo(lcHttp).noquote() << call.spec.method << call.redactedUrl << "failed:" << result.error().toString();

    if (!m_config.observer)
        return;
    RequestRecord r;
    r.operation = operationName(call.spec.operation);
    r.method = call.spec.method;
    r.bucket = call.spec.bucket;
    r.key = call.spec.key;
    r.versionId = call.spec.versionId;
    r.url = call.redactedUrl;
    r.mutating = isMutating(call.spec.operation);
    r.attempt = call.attempt;
    r.startedAt = call.startedAt;
    r.durationMs = ms;
    r.bytesSent = call.spec.body.size();
    r.bytesReceived = body.size();
    r.httpStatus = status;
    r.requestId = QString::fromLatin1(requestId);
    r.ok = result.has_value();
    if (!r.ok)
        r.error = result.error();
    if (m_config.captureResponseBodyBytes > 0)
        r.responseBody = body.left(m_config.captureResponseBodyBytes);
    m_config.observer->onRequestFinished(r);
}

void RequestPipeline::finish(const std::shared_ptr<Call>& call, Result<RawResponse> result)
{
    if (call->finished)
        return;
    call->finished = true;
    m_calls.erase(call->id);
    if (call->done)
        call->done(std::move(result));
}

void RequestPipeline::abort(quint64 id)
{
    const auto it = m_calls.find(id);
    if (it == m_calls.end())
        return;
    const std::shared_ptr<Call> call = it->second;
    call->aborted = true;
    if (call->retryTimer)
        call->retryTimer->deleteLater();
    if (call->reply) {
        call->reply->abort();   // → finished → Cancelled
        return;
    }
    // Not on the wire yet (waiting for credentials): complete right away, but
    // still asynchronously.
    QTimer::singleShot(0, this, [this, call] {
        finish(call, Error::make(ErrorCategory::Cancelled, QStringLiteral("request cancelled")));
    });
}

void RequestPipeline::abortAll()
{
    std::vector<quint64> ids;
    for (const auto& [id, call] : m_calls)
        ids.push_back(id);
    for (const quint64 id : ids)
        abort(id);
}

} // namespace qbucket::s3
