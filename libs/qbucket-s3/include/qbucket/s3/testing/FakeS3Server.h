#pragma once

#include <qbucket/s3/Credentials.h>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QRegularExpression>
#include <QUrl>

#include <functional>
#include <memory>

// FakeS3Server — an in-process, scriptable HTTP/1.1 server for testing S3
// clients, with fault injection (target qbucket::s3_testsupport, always a
// static library; depends on Qt Network, not on Qt Test).
//
// It answers what it is told to answer: routes match method + path (+ query
// keys / predicate) and return canned or computed responses, optionally with
// faults (delays, stalls, truncated bodies, wrong Content-Length, dropped
// connections). Every request is recorded with its raw bytes, and SigV4
// signatures can be verified from exactly what arrived on the wire.
//
// Thread affinity: lives in the thread that created it; that thread must run
// an event loop (QTRY_*, QSignalSpy::wait, …). Clients may be on any thread.
namespace qbucket::s3::testing {

using RawHeaders = QList<QPair<QByteArray, QByteArray>>;

struct RecordedRequest {
    QByteArray method;
    QByteArray target;      // raw request-target, e.g. "/bkt/a%20b?versioning"
    QByteArray path;        // target before '?'
    QByteArray query;       // target after '?', still encoded
    RawHeaders headers;     // as received, in order
    QByteArray body;
    int connection = 0;     // 1-based index of the TCP connection it came on

    // First header with this name (case-insensitive); null if absent.
    [[nodiscard]] QByteArray header(const QByteArray& name) const;
    [[nodiscard]] bool hasQueryItem(const QByteArray& name) const;
    // Raw (still encoded) value of a query item; null if absent.
    [[nodiscard]] QByteArray queryItem(const QByteArray& name) const;
};

struct FakeResponse {
    int status = 200;
    RawHeaders headers;
    QByteArray body;

    // --- faults ---
    int delayMs = 0;                     // wait before sending anything
    qint64 closeAfterBodyBytes = -1;     // send headers + N body bytes, then drop the connection
    qint64 stallAfterBodyBytes = -1;     // send headers + N body bytes, then go silent
    qint64 declaredContentLength = -1;   // Content-Length to announce instead of body.size()
    bool closeConnection = false;        // "Connection: close" and close after this response
    bool dropWithoutResponse = false;    // read the request, close without answering

    [[nodiscard]] static FakeResponse ok(const QByteArray& body = {},
                                         const QByteArray& contentType = "application/xml");
    [[nodiscard]] static FakeResponse s3Error(int status, const QByteArray& code,
                                              const QByteArray& message = "fake error");
    FakeResponse& withHeader(const QByteArray& name, const QByteArray& value);
};

struct Matcher {
    QByteArray method;                       // empty: any
    QRegularExpression path;                 // matched against the raw path; invalid/empty: any
    QList<QByteArray> requiredQueryKeys;     // all must be present
    std::function<bool(const RecordedRequest&)> predicate;   // optional
};

class FakeS3Server : public QObject {
    Q_OBJECT

public:
    explicit FakeS3Server(QObject* parent = nullptr);
    ~FakeS3Server() override;

    // Listens on 127.0.0.1 on a free port.
    bool listen();
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QUrl endpointUrl() const;      // http://127.0.0.1:<port>

    // Routes are tried in registration order; the first match answers. A
    // route returns its responses in sequence (the last one repeats) and is
    // used up after `times` matches (-1: unlimited).
    void addRoute(const Matcher& matcher, const QList<FakeResponse>& responses, int times = -1);
    void on(const QByteArray& method, const QString& pathPattern, const FakeResponse& response,
            int times = -1);
    // Computes the response when no route matched.
    void setFallbackHandler(std::function<FakeResponse(const RecordedRequest&)> handler);
    void clearRoutes();

    // Recompute SigV4 (header or presigned-query form) from the received
    // bytes; a mismatch is answered with 403 SignatureDoesNotMatch carrying
    // the server-side canonical request. Payload hashes are checked against
    // the body unless UNSIGNED-PAYLOAD.
    void setVerifySignatures(const Credentials& credentials, const QByteArray& region);

    // Shift the server clock (Date header, signature time check) — clock-skew tests.
    void setClockOffsetSecs(qint64 offset);

    [[nodiscard]] QList<RecordedRequest> requests() const;
    [[nodiscard]] int requestCount() const;
    [[nodiscard]] int connectionCount() const;     // TCP connections accepted so far
    void clearRequests();

Q_SIGNALS:
    void requestReceived(int index);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qbucket::s3::testing
