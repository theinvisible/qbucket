#include "client/RequestPipeline.h"

#include "client/FileRangeDevice.h"
#include "client/OneShotBody.h"
#include "core/ErrorMapping.h"
#include "core/HttpDate.h"
#include "core/Mailbox.h"
#include "logging/Redaction.h"
#include "xml/XmlParsers.h"

#include <qbucket/s3/Logging.h>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
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
    bool throttled = false;

    // Current attempt's streamed body; owned by its reply.
    QPointer<FileRangeDevice> upload;
    // Current attempt's sink. Undecided until the status is known: a 2xx
    // body goes to the file, anything else into errorBody.
    enum class SinkMode : quint8 { Undecided, File, Memory };
    std::unique_ptr<QFile> sink;
    std::unique_ptr<QCryptographicHash> sinkHash;
    qint64 sinkWritten = 0;
    SinkMode sinkMode = SinkMode::Undecided;
    QByteArray errorBody;
    QString localError;   // a local file failed: LocalIo, never retried
};

namespace {

// QNAM's read buffer for sink downloads: reading stops while the disk is
// behind, nothing piles up in memory.
constexpr qint64 kSinkReadBufferBytes = Q_INT64_C(1) * 1024 * 1024;

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

    // The provider may answer from any thread; continue on ours. Created
    // here, not in the constructor: the pipeline may have been moved to a
    // worker thread since.
    if (!m_mailbox)
        m_mailbox = Mailbox::create();
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
    // Streamed bodies are not hashed twice: Content-MD5 protects them.
    ctx.unsignedPayload = spec.file.has_value();
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
    if (!spec.body.isEmpty() || spec.file || !spec.contentType.isEmpty())
        in.headers.append({"Content-Type", spec.contentType});
    if (spec.file) {
        if (ctx.unsignedPayload && ctx.https) {
            in.payloadHash = SigV4Signer::kUnsignedPayload;
        } else if (!spec.file->sha256Hex.isEmpty()) {
            in.payloadHash = spec.file->sha256Hex;
        } else {
            finish(call, Error::make(ErrorCategory::InvalidRequest,
                                     QStringLiteral("this endpoint needs the SHA-256 of a streamed "
                                                    "body (plain HTTP or no UNSIGNED-PAYLOAD)")));
            return;
        }
    } else if (spec.body.isEmpty())
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
    req.setTransferTimeout(spec.serverWork ? std::max(m_config.serverWorkTimeoutMs, m_config.transferTimeoutMs)
                                           : m_config.transferTimeoutMs);
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

    // Fresh body and sink for every attempt: a retry sends / writes the same
    // bytes again from the start.
    FileRangeDevice* upload = nullptr;
    if (spec.file) {
        upload = new FileRangeDevice(spec.file->path, spec.file->offset, spec.file->length);
        if (!upload->open(QIODevice::ReadOnly)) {
            const QString why = upload->failure();
            delete upload;
            Error e = Error::make(ErrorCategory::LocalIo, why);
            e.resource = spec.key.isEmpty() ? spec.bucket : spec.bucket + u'/' + spec.key;
            finish(call, e);
            return;
        }
        req.setHeader(QNetworkRequest::ContentLengthHeader, spec.file->length);
        // Stream from the device; QNAM would otherwise read it all first.
        req.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
    }
    call->sink.reset();
    call->sinkHash.reset();
    call->sinkWritten = 0;
    call->sinkMode = Call::SinkMode::Undecided;
    call->errorBody.clear();
    if (!spec.sinkPath.isEmpty()) {
        auto file = std::make_unique<QFile>(spec.sinkPath);
        // ReadWrite: created if missing, never truncated (other ranges of
        // the same file may already be there).
        const QIODevice::OpenMode mode = spec.sinkCreate ? QIODevice::ReadWrite
                                                         : QIODevice::ReadWrite | QIODevice::ExistingOnly;
        if (!file->open(mode) || !file->seek(spec.sinkOffset)) {
            delete upload;
            Error e = Error::make(ErrorCategory::LocalIo,
                                  QStringLiteral("%1: %2").arg(spec.sinkPath, file->errorString()));
            e.resource = spec.key.isEmpty() ? spec.bucket : spec.bucket + u'/' + spec.key;
            finish(call, e);
            return;
        }
        call->sink = std::move(file);
        call->sinkHash = std::make_unique<QCryptographicHash>(QCryptographicHash::Md5);
    }

    call->startedAt = QDateTime::currentDateTimeUtc();
    call->timer.start();

    // Non-idempotent: a body QNAM can't rewind, so it never sends the request
    // a second time by itself (OneShotBody) — even an empty one.
    OneShotBody* oneShot = nullptr;
    if (!upload && !spec.idempotent && spec.method != "HEAD") {
        oneShot = new OneShotBody(spec.body);
        oneShot->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        req.setHeader(QNetworkRequest::ContentLengthHeader, spec.body.size());
        req.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
    }

    QNetworkReply* reply = nullptr;
    if (spec.method == "HEAD")
        reply = nam()->head(req);
    else if (upload)
        reply = nam()->sendCustomRequest(req, spec.method, upload);
    else if (oneShot)
        reply = nam()->sendCustomRequest(req, spec.method, oneShot);
    else
        reply = nam()->sendCustomRequest(req, spec.method, spec.body);
    call->reply = reply;
    if (upload) {
        upload->setParent(reply);   // must outlive the reply; goes with it
        call->upload = upload;
    }
    if (oneShot)
        oneShot->setParent(reply);

    if (call->sink) {
        reply->setReadBufferSize(kSinkReadBufferBytes);
        connect(reply, &QNetworkReply::readyRead, this, [this, call] { drainToSink(call); });
    }
    connect(reply, &QNetworkReply::downloadProgress, this, [this, call](qint64 received, qint64 total) {
        if (call->sink && call->sinkMode != Call::SinkMode::Memory) {
            if (call->spec.progress && call->sinkMode == Call::SinkMode::File)
                call->spec.progress(received, total);
            return;
        }
        if (received > m_config.maxResponseBytes && !call->tooLarge && call->reply) {
            call->tooLarge = true;
            call->reply->abort();
        }
    });
    if (upload && spec.progress) {
        // Forward only what grows: Qt 6.12 on Windows reports 0 again once
        // the upload is over, and a resend by QNAM starts over from 0.
        auto reported = std::make_shared<qint64>(-1);
        connect(reply, &QNetworkReply::uploadProgress, this, [call, reported](qint64 sent, qint64) {
            if (!call->spec.progress || !call->spec.file || sent <= *reported)
                return;
            *reported = sent;
            call->spec.progress(sent, call->spec.file->length);
        });
    }
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

void RequestPipeline::drainToSink(const std::shared_ptr<Call>& call)
{
    QNetworkReply* reply = call->reply;
    if (!reply || !call->sink || !call->localError.isEmpty())
        return;
    if (call->sinkMode == Call::SinkMode::Undecided) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        call->sinkMode = status >= 200 && status < 300 ? Call::SinkMode::File : Call::SinkMode::Memory;
    }
    while (reply->bytesAvailable() > 0) {
        const QByteArray chunk = reply->read(qint64(256) * 1024);
        if (chunk.isEmpty())
            break;
        if (call->sinkMode == Call::SinkMode::Memory) {
            call->errorBody += chunk;
            if (call->errorBody.size() > m_config.maxResponseBytes && !call->tooLarge) {
                call->tooLarge = true;
                reply->abort();
                return;
            }
            continue;
        }
        if (call->sink->write(chunk) != chunk.size()) {
            call->localError = QStringLiteral("%1: %2").arg(call->spec.sinkPath, call->sink->errorString());
            reply->abort();
            return;
        }
        call->sinkHash->addData(chunk);
        call->sinkWritten += chunk.size();
    }
}

void RequestPipeline::onFinished(const std::shared_ptr<Call>& call)
{
    QNetworkReply* reply = call->reply;
    if (!reply || call->finished)
        return;
    if (call->sink && reply->isOpen())
        drainToSink(call);   // whatever arrived after the last readyRead
    reply->deleteLater();
    call->reply = nullptr;

    const RequestSpec& spec = call->spec;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();

    RawResponse raw;
    raw.status = status;
    raw.headers = reply->rawHeaderPairs();
    if (call->sink) {
        raw.body = call->errorBody;
        if (reply->isOpen())
            raw.body += reply->readAll();
        if (call->sinkMode == Call::SinkMode::File) {
            raw.sinkBytes = call->sinkWritten;
            raw.sinkMd5 = call->sinkHash->result();
        }
        if (!call->sink->flush() && call->localError.isEmpty())
            call->localError = QStringLiteral("%1: %2").arg(spec.sinkPath, call->sink->errorString());
        call->sink->close();
    } else if (reply->isOpen()) {
        raw.body = reply->readAll();
    }
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
    } else if (!call->localError.isEmpty()) {
        result = withContext(Error::make(ErrorCategory::LocalIo, call->localError));
    } else if (call->upload && call->upload->failed()) {
        // The body file changed under us; the server has at most a partial,
        // rejected body.
        result = withContext(Error::make(ErrorCategory::LocalIo, call->upload->failure()));
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
        if (raw.header("x-amz-delete-marker") == "true") {
            e.deleteMarker = true;
            e.versionId = QString::fromLatin1(raw.header("x-amz-version-id"));
            if (status == 405)   // HEAD / GET on a delete-marker version
                e.category = ErrorCategory::NotFound;
        }
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

    // 1b. HEAD responses carry no body: a skewed clock shows up as a bare
    //     403. If the server's Date is far off our corrected time, that is
    //     the likelier cause — re-sign once with the server's time.
    if (error.httpStatus == 403 && error.code.isEmpty() && raw.body.isEmpty() && !call->skewRetried) {
        const QDateTime serverTime = parseHttpDate(raw.header("date"));
        if (serverTime.isValid() && std::abs(correctedNow().secsTo(serverTime)) > 300) {   // 5 min
            const qint64 offset = QDateTime::currentDateTimeUtc().secsTo(serverTime);
            m_config.clockOffsetSecs->store(offset);
            qCWarning(lcSigning) << "bare 403 with a server clock" << offset << "s off: re-signing";
            Q_EMIT clockSkewDetected(offset);
            call->skewRetried = true;
            retryLater(call, 0, QStringLiteral("probable clock skew"));
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
    if (error.httpStatus == 503 || error.code == QLatin1String("SlowDown"))
        call->throttled = true;
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
    r.bytesSent = call.spec.file ? call.spec.file->length : call.spec.body.size();
    r.bytesReceived = result ? result->sinkBytes + body.size() : body.size();
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
    if (result)
        result->throttled = call->throttled;
    call->sink.reset();
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
    ids.reserve(m_calls.size());
    for (const auto& [id, call] : m_calls)
        ids.push_back(id);
    for (const quint64 id : ids)
        abort(id);
}

} // namespace qbucket::s3
