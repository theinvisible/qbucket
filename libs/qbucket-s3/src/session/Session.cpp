#include <qbucket/s3/Session.h>

#include "client/S3Client.h"

#include <qbucket/s3/Logging.h>
#include <qbucket/s3/Version.h>

#include <QMutex>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <optional>
#include <unordered_map>

namespace qbucket::s3 {
namespace {

// Carries one callback into the context's thread. Lives in that thread;
// deleted by whoever removed its entry from the registry — its own delivery,
// or the Session's teardown. The worker therefore never posts to a relay
// that could already be gone.
class CallbackRelay final : public QObject {
public:
    QPointer<QObject> context;
};

Error cancelledError()
{
    return Error::make(ErrorCategory::Cancelled, QStringLiteral("request cancelled"));
}

} // namespace

struct Session::Impl {
    struct Pending {
        CallbackRelay* relay = nullptr;
        quint64 clientOp = 0;                 // 0 until started on the worker
        std::function<void()> deliverCancelled;
    };

    Session* q = nullptr;
    SessionOptions options;
    QThread* worker = nullptr;
    S3Client* client = nullptr;   // lives on `worker`

    QMutex mutex;                 // guards pending, nextId, stopped
    std::unordered_map<quint64, Pending> pending;
    quint64 nextId = 1;
    bool stopped = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    // Posts the result to the relay's thread; the relay deletes itself.
    template <typename T>
    static void post(CallbackRelay* relay, const std::shared_ptr<std::atomic<bool>>& alive,
                     Callback<T> cb, Result<T> result)
    {
        QMetaObject::invokeMethod(
            relay,
            [relay, alive, cb = std::move(cb), result = std::move(result)]() mutable {
                if (alive->load() && relay->context)
                    cb(std::move(result));
                relay->deleteLater();
            },
            Qt::QueuedConnection);
    }

    // Whoever takes an entry out of `pending` owns its delivery — exactly once.
    std::optional<Pending> take(quint64 id)
    {
        QMutexLocker lock(&mutex);
        const auto it = pending.find(id);
        if (it == pending.end())
            return std::nullopt;
        Pending p = std::move(it->second);
        pending.erase(it);
        return p;
    }

    template <typename T>
    void deliver(quint64 id, Result<T> result, Callback<T> cb)
    {
        if (const std::optional<Pending> p = take(id))
            post(p->relay, alive, std::move(cb), std::move(result));
    }

    // Registers the request and queues `start(client, done)` on the worker.
    template <typename T, typename Start>
    RequestId submit(QObject* context, Callback<T> cb, Start start)
    {
        if (!context) {
            qCWarning(lcSession) << "request without a context object ignored";
            return RequestId::Invalid;
        }
        auto* relay = new CallbackRelay;
        relay->context = context;
        relay->moveToThread(context->thread());

        quint64 id = 0;
        bool accepted = false;
        {
            QMutexLocker lock(&mutex);
            id = nextId++;
            if (!stopped) {
                Pending p;
                p.relay = relay;
                p.deliverCancelled = [relay, alive = alive, cb] {
                    post<T>(relay, alive, cb, cancelledError());
                };
                pending.emplace(id, std::move(p));
                accepted = true;
            }
        }
        if (!accepted) {   // after shutdown()
            post<T>(relay, alive, std::move(cb), cancelledError());
            return RequestId(id);
        }

        QMetaObject::invokeMethod(
            client,
            [this, id, cb = std::move(cb), start = std::move(start)]() mutable {
                {
                    QMutexLocker lock(&mutex);
                    if (!pending.count(id))
                        return;   // cancelled before it started: nothing to send
                }
                const quint64 op = start(client, [this, id, cb](Result<T> r) {
                    deliver<T>(id, std::move(r), cb);
                });
                QMutexLocker lock(&mutex);
                const auto it = pending.find(id);
                if (it != pending.end())
                    it->second.clientOp = op;
                else
                    client->abort(op);   // cancelled while starting
            },
            Qt::QueuedConnection);
        return RequestId(id);
    }

    // Takes every pending entry. With `notify`, each gets Cancelled.
    void cancelEverything(bool notify)
    {
        std::unordered_map<quint64, Pending> all;
        {
            QMutexLocker lock(&mutex);
            all.swap(pending);
        }
        for (auto& [id, p] : all) {
            if (notify)
                p.deliverCancelled();
            else
                p.relay->deleteLater();
            if (p.clientOp != 0) {
                QMetaObject::invokeMethod(client, [c = client, op = p.clientOp] { c->abort(op); },
                                          Qt::QueuedConnection);
            }
        }
    }
};

Session::Session(SessionOptions options, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    Q_ASSERT(options.profile);
    Q_ASSERT(options.credentials);
    d->q = this;
    d->options = std::move(options);

    ClientConfig cfg;
    cfg.endpoint = d->options.endpoint;
    cfg.profile = d->options.profile;
    cfg.credentials = d->options.credentials;
    cfg.observer = d->options.observer;
    cfg.transferTimeoutMs = d->options.transferTimeoutMs;
    cfg.maxResponseBytes = d->options.maxInMemoryBytes;
    cfg.captureResponseBodyBytes = d->options.captureResponseBodyBytes;
    cfg.retry.maxAttempts = std::max(1, d->options.maxAttempts);
    cfg.retry.baseDelayMs = d->options.retryBaseDelayMs;
    cfg.userAgent = d->options.userAgent.isEmpty()
        ? QByteArray("qbucket-s3/" QBUCKET_S3_VERSION_STRING)
        : d->options.userAgent;

    d->worker = new QThread;
    d->worker->setObjectName(QStringLiteral("qbucket-s3 session"));
    d->client = new S3Client(std::move(cfg));
    d->client->moveToThread(d->worker);
    connect(d->worker, &QThread::finished, d->client, &QObject::deleteLater);
    connect(d->client, &S3Client::clockSkewDetected, this, &Session::clockSkewDetected);
    d->worker->start();
}

Session::~Session()
{
    d->alive->store(false);
    {
        QMutexLocker lock(&d->mutex);
        d->stopped = true;
    }
    d->cancelEverything(false);
    if (d->worker->isRunning()) {
        d->worker->quit();
        if (!d->worker->wait(2000)) {
            // Never terminate(): leak the worker and its state instead.
            qCWarning(lcSession) << "worker thread did not stop in time; leaking it";
            (void)d.release();
            return;
        }
    }
    delete d->worker;
}

const SessionOptions& Session::options() const
{
    return d->options;
}

void Session::cancel(RequestId id)
{
    const std::optional<Impl::Pending> p = d->take(quint64(id));
    if (!p)
        return;   // unknown, already finished or already cancelled
    p->deliverCancelled();
    // Not started yet: the queued start finds no entry and sends nothing.
    if (p->clientOp != 0) {
        QMetaObject::invokeMethod(d->client, [c = d->client, op = p->clientOp] { c->abort(op); },
                                  Qt::QueuedConnection);
    }
}

void Session::cancelAll()
{
    d->cancelEverything(true);
}

void Session::shutdown()
{
    {
        QMutexLocker lock(&d->mutex);
        d->stopped = true;
    }
    d->cancelEverything(true);
    if (!d->worker->isRunning()) {
        QMetaObject::invokeMethod(this, &Session::shutdownFinished, Qt::QueuedConnection);
        return;
    }
    connect(d->worker, &QThread::finished, this, &Session::shutdownFinished, Qt::UniqueConnection);
    // Every callback has its Cancelled already; whatever the worker still
    // has in flight is torn down with the client (no callbacks).
    d->worker->quit();
}

// --- requests ----------------------------------------------------------------------

RequestId Session::listBuckets(QObject* context, Callback<BucketList> callback)
{
    return d->submit<BucketList>(context, std::move(callback), [](S3Client* c, auto done) {
        return c->listBuckets(std::move(done));
    });
}

RequestId Session::listObjects(const ListObjectsParams& params, QObject* context,
                               Callback<ListPage> callback)
{
    return d->submit<ListPage>(context, std::move(callback), [params](S3Client* c, auto done) {
        return c->listObjects(params, std::move(done));
    });
}

RequestId Session::headBucket(const QString& bucket, QObject* context, Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback), [bucket](S3Client* c, auto done) {
        return c->headBucket(bucket, std::move(done));
    });
}

RequestId Session::getBucketLocation(const QString& bucket, QObject* context,
                                     Callback<QString> callback)
{
    return d->submit<QString>(context, std::move(callback), [bucket](S3Client* c, auto done) {
        return c->getBucketLocation(bucket, std::move(done));
    });
}

RequestId Session::headObject(const QString& bucket, const QString& key, const QString& versionId,
                              QObject* context, Callback<ObjectMeta> callback)
{
    return d->submit<ObjectMeta>(context, std::move(callback),
                                 [bucket, key, versionId](S3Client* c, auto done) {
                                     return c->headObject(bucket, key, versionId, std::move(done));
                                 });
}

RequestId Session::getObject(const QString& bucket, const QString& key, const QString& versionId,
                             QObject* context, Callback<ObjectData> callback)
{
    return d->submit<ObjectData>(context, std::move(callback),
                                 [bucket, key, versionId](S3Client* c, auto done) {
                                     return c->getObject(bucket, key, versionId, std::move(done));
                                 });
}

RequestId Session::putObject(const PutObjectParams& params, QObject* context,
                             Callback<PutObjectResult> callback)
{
    return d->submit<PutObjectResult>(context, std::move(callback), [params](S3Client* c, auto done) {
        return c->putObject(params, std::move(done));
    });
}

RequestId Session::deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                                QObject* context, Callback<DeleteObjectResult> callback)
{
    return d->submit<DeleteObjectResult>(context, std::move(callback),
                                         [bucket, key, versionId](S3Client* c, auto done) {
                                             return c->deleteObject(bucket, key, versionId, std::move(done));
                                         });
}

RequestId Session::deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                                 QObject* context, Callback<DeleteObjectsResult> callback)
{
    return d->submit<DeleteObjectsResult>(context, std::move(callback),
                                          [bucket, objects](S3Client* c, auto done) {
                                              return c->deleteObjects(bucket, objects, std::move(done));
                                          });
}

RequestId Session::createBucket(const CreateBucketParams& params, QObject* context,
                                Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback), [params](S3Client* c, auto done) {
        return c->createBucket(params, std::move(done));
    });
}

RequestId Session::deleteBucket(const QString& bucket, QObject* context, Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback), [bucket](S3Client* c, auto done) {
        return c->deleteBucket(bucket, std::move(done));
    });
}

RequestId Session::presign(const PresignRequest& request, QObject* context, Callback<QUrl> callback)
{
    return d->submit<QUrl>(context, std::move(callback), [request](S3Client* c, auto done) {
        return c->presign(request, std::move(done));
    });
}

} // namespace qbucket::s3
