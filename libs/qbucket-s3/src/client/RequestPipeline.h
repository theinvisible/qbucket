#pragma once

#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Operation.h>
#include <qbucket/s3/RequestObserver.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/providers/ProviderProfile.h>

#include "client/RetryPolicy.h"
#include "http/S3Uri.h"
#include "signing/SigV4Signer.h"

#include <QHash>
#include <QObject>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace qbucket::s3 {

class Mailbox;

struct ClientConfig {
    Endpoint endpoint;
    std::shared_ptr<const ProviderProfile> profile;
    std::shared_ptr<CredentialsProvider> credentials;
    std::shared_ptr<RequestObserver> observer;   // optional
    int transferTimeoutMs = 120'000;             // no progress for this long → Transient
    qint64 maxResponseBytes = Q_INT64_C(64) * 1024 * 1024;   // in-memory bodies
    int captureResponseBodyBytes = 0;            // > 0: RequestRecord::responseBody
    QByteArray userAgent = "qbucket-s3";
    RetryOptions retry;
    // Server time minus local time, learned from RequestTimeTooSkewed and
    // Date headers; shared with whoever presigns for this endpoint. Created
    // by the pipeline when null.
    std::shared_ptr<std::atomic<qint64>> clockOffsetSecs;
};

// One request as the client wants it sent.
struct RequestSpec {
    Operation operation = Operation::ListBuckets;
    QByteArray method = "GET";
    QString bucket;
    QString key;
    QString versionId;           // for records/logs only
    QString sourceBucket;        // server-side copies, for the capability check
    QueryParams query;
    HeaderList headers;          // all are signed
    QByteArray body;
    QByteArray contentType;      // required when body or file is set
    // Streamed body instead of `body`, re-read from the range start on every
    // attempt. UNSIGNED-PAYLOAD over TLS unless the profile vetoes it,
    // otherwise FileBody::sha256Hex (required then).
    std::optional<FileBody> file;
    // A 2xx response body goes to this file from sinkOffset on, not into
    // memory (and is not limited by maxResponseBytes); error documents are
    // still read into memory. Reopened and rewritten on every attempt.
    QString sinkPath;
    qint64 sinkOffset = 0;
    bool sinkCreate = true;      // false: the sink file must exist
    // Bytes sent (file bodies) or received (sinks) by the current attempt,
    // and the total (-1 if unknown). Called on the pipeline's thread.
    std::function<void(qint64 done, qint64 total)> progress;
    bool idempotent = true;
    // CopyObject / UploadPartCopy / CompleteMultipartUpload can answer 200
    // with an <Error> document. Never set for GetObject (user data).
    bool mayReturn200Error = false;
};

struct RawResponse {
    int status = 0;
    HeaderList headers;
    QByteArray body;
    // With RequestSpec::sinkPath: what went to the file.
    qint64 sinkBytes = 0;
    QByteArray sinkMd5;          // raw 16 bytes
    // At least one attempt was throttled (SlowDown / 503) before this answer.
    bool throttled = false;
    [[nodiscard]] QByteArray header(const QByteArray& name) const;   // case-insensitive
};

// Sends RequestSpecs: credentials, provider hooks, URI, SigV4, QNAM, and
// classification of the outcome into RawResponse or Error. Owns its QNAM.
//
// Thread affinity: lives on one thread (the client's); every member must be
// called there. Callbacks run there too, never synchronously from execute().
class QBUCKET_S3_EXPORT RequestPipeline : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(Result<RawResponse>)>;

    explicit RequestPipeline(ClientConfig config, QObject* parent = nullptr);
    ~RequestPipeline() override;

    // Returns an id for abort(); 0 is never used.
    quint64 execute(RequestSpec spec, Done done);
    void abort(quint64 id);
    void abortAll();

    [[nodiscard]] const ClientConfig& config() const { return m_config; }

    // Signing time: local clock corrected by the learned offset.
    [[nodiscard]] QDateTime correctedNow() const;

    // Current credentials (cached, or fetched from the provider). Never
    // invokes `next` synchronously from a provider on another thread.
    void withCredentials(std::function<void(Result<Credentials>)> next);

    // Endpoint for a bucket, with a learned region applied.
    [[nodiscard]] Endpoint endpointFor(const QString& bucket) const;

Q_SIGNALS:
    // The local clock differs from the server's by offsetSecs; signing has
    // been corrected (never ask the user to fix their clock).
    void clockSkewDetected(qint64 offsetSecs);

private:
    struct Call;

    void attempt(const std::shared_ptr<Call>& call);
    void start(const std::shared_ptr<Call>& call, const Credentials& credentials);
    void drainToSink(const std::shared_ptr<Call>& call);
    void onFinished(const std::shared_ptr<Call>& call);
    // Retry / re-sign / re-route a failed attempt; false: give up.
    bool handleFailure(const std::shared_ptr<Call>& call, const RawResponse& raw, const Error& error);
    void retryLater(const std::shared_ptr<Call>& call, int delayMs, const QString& why);
    void learnClockOffset(const QDateTime& serverTime);
    void finish(const std::shared_ptr<Call>& call, Result<RawResponse> result);
    void notifyObserver(const Call& call, const Result<RawResponse>& result, int status,
                        const QByteArray& body, const QByteArray& requestId) const;
    QNetworkAccessManager* nam();

    ClientConfig m_config;
    RetryPolicy m_retry;
    QHash<QString, QString> m_bucketRegions;   // learned from redirects
    QNetworkAccessManager* m_nam = nullptr;
    std::shared_ptr<Mailbox> m_mailbox;   // created on first use, in our thread
    std::map<quint64, std::shared_ptr<Call>> m_calls;
    quint64 m_nextId = 1;

    std::optional<Credentials> m_credentials;
    bool m_fetchingCredentials = false;
    std::vector<std::function<void(Result<Credentials>)>> m_credentialWaiters;
};

} // namespace qbucket::s3
