#pragma once

#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Operation.h>
#include <qbucket/s3/RequestObserver.h>
#include <qbucket/s3/Result.h>
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
    QueryParams query;
    HeaderList headers;          // all are signed
    QByteArray body;
    QByteArray contentType;      // required when body is set
    bool idempotent = true;
    // CopyObject / UploadPartCopy / CompleteMultipartUpload can answer 200
    // with an <Error> document. Never set for GetObject (user data).
    bool mayReturn200Error = false;
};

struct RawResponse {
    int status = 0;
    HeaderList headers;
    QByteArray body;
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

Q_SIGNALS:
    // The local clock differs from the server's by offsetSecs; signing has
    // been corrected (never ask the user to fix their clock).
    void clockSkewDetected(qint64 offsetSecs);

private:
    struct Call;

    void attempt(const std::shared_ptr<Call>& call);
    void withCredentials(std::function<void(Result<Credentials>)> next);
    void start(const std::shared_ptr<Call>& call, const Credentials& credentials);
    void onFinished(const std::shared_ptr<Call>& call);
    // Retry / re-sign / re-route a failed attempt; false: give up.
    bool handleFailure(const std::shared_ptr<Call>& call, const RawResponse& raw, const Error& error);
    void retryLater(const std::shared_ptr<Call>& call, int delayMs, const QString& why);
    void learnClockOffset(const QDateTime& serverTime);
    [[nodiscard]] Endpoint endpointFor(const QString& bucket) const;
    void finish(const std::shared_ptr<Call>& call, Result<RawResponse> result);
    void notifyObserver(const Call& call, const Result<RawResponse>& result, int status,
                        const QByteArray& body, const QByteArray& requestId) const;
    QNetworkAccessManager* nam();

    ClientConfig m_config;
    RetryPolicy m_retry;
    QHash<QString, QString> m_bucketRegions;   // learned from redirects
    QNetworkAccessManager* m_nam = nullptr;
    std::shared_ptr<Mailbox> m_mailbox;
    std::map<quint64, std::shared_ptr<Call>> m_calls;
    quint64 m_nextId = 1;

    std::optional<Credentials> m_credentials;
    bool m_fetchingCredentials = false;
    std::vector<std::function<void(Result<Credentials>)>> m_credentialWaiters;
};

} // namespace qbucket::s3
