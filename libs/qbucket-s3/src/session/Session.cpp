#include <qbucket/s3/Session.h>

#include "client/S3Client.h"

#include <qbucket/s3/Logging.h>
#include <qbucket/s3/Version.h>

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <optional>
#include <unordered_map>
#include <vector>

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
    // One worker thread with its own S3Client + QNAM. Lane 0 is the browse
    // lane; transfer lanes (1..transferLanes) are started on demand.
    struct Lane {
        QThread* thread = nullptr;
        S3Client* client = nullptr;   // lives on `thread`
        int load = 0;                 // pending requests (guarded by mutex)
    };
    struct Pending {
        CallbackRelay* relay = nullptr;
        Lane* lane = nullptr;
        quint64 clientOp = 0;                 // 0 until started on the worker
        std::function<void()> deliverCancelled;
    };

    Session* q = nullptr;
    SessionOptions options;
    ClientConfig baseConfig;      // per lane; the clock offset is shared

    QMutex mutex;                 // guards pending, nextId, stopped, lanes, capabilities
    std::vector<std::unique_ptr<Lane>> lanes;
    QHash<QString, CapabilitySet> capabilities;   // "" = server scope, else the bucket
    std::unordered_map<quint64, Pending> pending;
    quint64 nextId = 1;
    bool stopped = false;
    bool shutdownRequested = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    // Called with the mutex held (or before the Session is shared).
    Lane* createLane()
    {
        auto lane = std::make_unique<Lane>();
        const int index = int(lanes.size());
        lane->thread = new QThread;
        lane->thread->setObjectName(index == 0 ? QStringLiteral("qbucket-s3 session")
                                               : QStringLiteral("qbucket-s3 transfer %1").arg(index));
        lane->client = new S3Client(baseConfig);
        lane->client->moveToThread(lane->thread);
        QObject::connect(lane->thread, &QThread::finished, lane->client, &QObject::deleteLater);
        QObject::connect(lane->client, &S3Client::clockSkewDetected, q, &Session::clockSkewDetected);
        QObject::connect(lane->client, &S3Client::capabilityLearned, q, &Session::capabilityLearned);
        for (const CapabilitySet& set : std::as_const(capabilities)) {
            QMetaObject::invokeMethod(lane->client, [c = lane->client, set] { c->setCapabilities(set); },
                                      Qt::QueuedConnection);
        }
        lane->thread->start();
        lanes.push_back(std::move(lane));
        return lanes.back().get();
    }

    // The mutex is held. Browse requests stay on lane 0; transfers go to the
    // least busy transfer lane, a new one while all are busy.
    Lane* pickLane(bool transfer)
    {
        if (!transfer || options.transferLanes <= 0)
            return lanes.front().get();
        Lane* best = nullptr;
        for (size_t i = 1; i < lanes.size(); ++i) {
            if (!best || lanes[i]->load < best->load)
                best = lanes[i].get();
        }
        if ((!best || best->load > 0) && int(lanes.size()) - 1 < options.transferLanes)
            best = createLane();
        return best ? best : lanes.front().get();
    }

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
        --p.lane->load;
        return p;
    }

    template <typename T>
    void deliver(quint64 id, Result<T> result, Callback<T> cb)
    {
        if (const std::optional<Pending> p = take(id))
            post(p->relay, alive, std::move(cb), std::move(result));
    }

    // Worker side of a ProgressCallback: at most every 100 ms (and the
    // final value), posted to the relay while the request is still pending —
    // so always before its result and never to a deleted relay.
    S3Client::Progress progressFor(quint64 id, ProgressCallback cb)
    {
        if (!cb)
            return {};
        auto timer = std::make_shared<QElapsedTimer>();
        return [self = this, id, cb = std::move(cb), timer](qint64 done, qint64 total) {
            if (timer->isValid() && timer->elapsed() < 100 && done != total)
                return;
            timer->start();
            QMutexLocker lock(&self->mutex);
            const auto it = self->pending.find(id);
            if (it == self->pending.end())
                return;
            CallbackRelay* relay = it->second.relay;
            QMetaObject::invokeMethod(
                relay,
                [relay, alive = self->alive, cb, done, total] {
                    if (alive->load() && relay->context)
                        cb(done, total);
                },
                Qt::QueuedConnection);
        };
    }

    // Registers the request and queues `start(client, done, progress)` on a
    // lane's worker.
    template <typename T, typename Start>
    RequestId submit(QObject* context, Callback<T> cb, Start start, bool transfer = false,
                     ProgressCallback progress = {})
    {
        if (!context) {
            qCWarning(lcSession) << "request without a context object ignored";
            return RequestId::Invalid;
        }
        auto* relay = new CallbackRelay;
        relay->context = context;
        relay->moveToThread(context->thread());

        quint64 id = 0;
        Lane* lane = nullptr;
        {
            QMutexLocker lock(&mutex);
            id = nextId++;
            if (!stopped) {
                lane = pickLane(transfer);
                ++lane->load;
                Pending p;
                p.relay = relay;
                p.lane = lane;
                p.deliverCancelled = [relay, alive = alive, cb] {
                    post<T>(relay, alive, cb, cancelledError());
                };
                pending.emplace(id, std::move(p));
            }
        }
        if (!lane) {   // after shutdown()
            post<T>(relay, alive, std::move(cb), cancelledError());
            return RequestId(id);
        }

        // Impl and the lanes outlive the workers (the destructor joins
        // them), so the raw pointers are safe on the worker thread.
        Impl* const self = this;
        S3Client* const client = lane->client;
        QMetaObject::invokeMethod(
            client,
            [self, client, id, cb = std::move(cb), start = std::move(start),
             progress = progressFor(id, std::move(progress))]() mutable {
                {
                    QMutexLocker lock(&self->mutex);
                    if (self->pending.find(id) == self->pending.end())
                        return;   // cancelled before it started: nothing to send
                }
                const quint64 op = start(client, [self, id, cb](Result<T> r) {
                    self->deliver<T>(id, std::move(r), cb);
                }, std::move(progress));
                QMutexLocker lock(&self->mutex);
                const auto it = self->pending.find(id);
                if (it != self->pending.end())
                    it->second.clientOp = op;
                else
                    client->abort(op);   // cancelled while starting
            },
            Qt::QueuedConnection);
        return RequestId(id);
    }

    static void abortOn(const Pending& p)
    {
        if (p.clientOp == 0)
            return;   // not started: the queued start finds no entry and sends nothing
        QMetaObject::invokeMethod(p.lane->client, [c = p.lane->client, op = p.clientOp] { c->abort(op); },
                                  Qt::QueuedConnection);
    }

    // Takes every pending entry. With `notify`, each gets Cancelled.
    void cancelEverything(bool notify)
    {
        std::unordered_map<quint64, Pending> all;
        {
            QMutexLocker lock(&mutex);
            all.swap(pending);
            for (auto& lane : lanes)
                lane->load = 0;
        }
        for (auto& [id, p] : all) {
            if (notify)
                p.deliverCancelled();
            else
                p.relay->deleteLater();
            abortOn(p);
        }
    }

    std::vector<QThread*> threads()
    {
        QMutexLocker lock(&mutex);
        std::vector<QThread*> out;
        out.reserve(lanes.size());
        for (const auto& lane : lanes)
            out.push_back(lane->thread);
        return out;
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

    ClientConfig& cfg = d->baseConfig;
    cfg.endpoint = d->options.endpoint;
    cfg.profile = d->options.profile;
    cfg.credentials = d->options.credentials;
    cfg.observer = d->options.observer;
    cfg.transferTimeoutMs = d->options.transferTimeoutMs;
    cfg.maxResponseBytes = d->options.maxInMemoryBytes;
    cfg.captureResponseBodyBytes = d->options.captureResponseBodyBytes;
    cfg.retry.maxAttempts = std::max(1, d->options.maxAttempts);
    cfg.retry.baseDelayMs = d->options.retryBaseDelayMs;
    cfg.clockOffsetSecs = std::make_shared<std::atomic<qint64>>(d->options.initialClockOffsetSecs);
    cfg.userAgent = d->options.userAgent.isEmpty()
        ? QByteArray("qbucket-s3/" QBUCKET_S3_VERSION_STRING)
        : d->options.userAgent;

    QMutexLocker lock(&d->mutex);
    d->createLane();
}

Session::~Session()
{
    d->alive->store(false);
    {
        QMutexLocker lock(&d->mutex);
        d->stopped = true;
    }
    d->cancelEverything(false);
    const std::vector<QThread*> threads = d->threads();
    for (QThread* t : threads)
        t->quit();
    QDeadlineTimer deadline(2000);
    for (QThread* t : threads) {
        if (t->isRunning() && !t->wait(deadline)) {
            // Never terminate(): leak the workers and their state instead.
            qCWarning(lcSession) << "worker thread did not stop in time; leaking it";
            [[maybe_unused]] Impl* const leaked = d.release();
            return;
        }
    }
    for (QThread* t : threads)
        delete t;
}

const SessionOptions& Session::options() const
{
    return d->options;
}

bool Session::requiresPayloadHash() const
{
    if (!d->options.endpoint.isHttps())
        return true;
    RequestContext ctx;
    ctx.operation = Operation::PutObject;
    ctx.method = "PUT";
    ctx.https = true;
    ctx.unsignedPayload = true;
    d->options.profile->prepareRequest(ctx);
    return !ctx.unsignedPayload;
}

void Session::cancel(RequestId id)
{
    const std::optional<Impl::Pending> p = d->take(quint64(id));
    if (!p)
        return;   // unknown, already finished or already cancelled
    p->deliverCancelled();
    Impl::abortOn(*p);
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
        if (d->shutdownRequested)
            return;
        d->shutdownRequested = true;
    }
    d->cancelEverything(true);
    std::vector<QThread*> running;
    for (QThread* t : d->threads()) {
        if (t->isRunning())
            running.push_back(t);
    }
    if (running.empty()) {
        QMetaObject::invokeMethod(this, &Session::shutdownFinished, Qt::QueuedConnection);
        return;
    }
    // Every callback has its Cancelled already; whatever the workers still
    // have in flight is torn down with their clients (no callbacks).
    auto remaining = std::make_shared<int>(int(running.size()));
    for (QThread* t : running) {
        connect(t, &QThread::finished, this, [this, remaining] {
            if (--*remaining == 0)
                Q_EMIT shutdownFinished();
        });
        t->quit();
    }
}

// --- requests ----------------------------------------------------------------------

RequestId Session::listBuckets(QObject* context, Callback<BucketList> callback)
{
    return d->submit<BucketList>(context, std::move(callback), [](S3Client* c, auto done, auto /*progress*/) {
        return c->listBuckets(std::move(done));
    });
}

RequestId Session::listObjects(const ListObjectsParams& params, QObject* context,
                               Callback<ListPage> callback)
{
    return d->submit<ListPage>(context, std::move(callback), [params](S3Client* c, auto done, auto /*progress*/) {
        return c->listObjects(params, std::move(done));
    });
}

RequestId Session::headBucket(const QString& bucket, QObject* context, Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback), [bucket](S3Client* c, auto done, auto /*progress*/) {
        return c->headBucket(bucket, std::move(done));
    });
}

RequestId Session::getBucketLocation(const QString& bucket, QObject* context,
                                     Callback<QString> callback)
{
    return d->submit<QString>(context, std::move(callback), [bucket](S3Client* c, auto done, auto /*progress*/) {
        return c->getBucketLocation(bucket, std::move(done));
    });
}

RequestId Session::headObject(const QString& bucket, const QString& key, const QString& versionId,
                              QObject* context, Callback<ObjectMeta> callback)
{
    return d->submit<ObjectMeta>(context, std::move(callback),
                                 [bucket, key, versionId](S3Client* c, auto done, auto /*progress*/) {
                                     return c->headObject(bucket, key, versionId, std::move(done));
                                 });
}

RequestId Session::headObject(const HeadObjectParams& params, QObject* context, Callback<ObjectMeta> callback)
{
    return d->submit<ObjectMeta>(context, std::move(callback), [params](S3Client* c, auto done, auto /*progress*/) {
        return c->headObject(params, std::move(done));
    });
}

RequestId Session::getObject(const QString& bucket, const QString& key, const QString& versionId,
                             QObject* context, Callback<ObjectData> callback)
{
    return d->submit<ObjectData>(context, std::move(callback),
                                 [bucket, key, versionId](S3Client* c, auto done, auto /*progress*/) {
                                     return c->getObject(bucket, key, versionId, std::move(done));
                                 });
}

RequestId Session::putObject(const PutObjectParams& params, QObject* context,
                             Callback<PutObjectResult> callback, ProgressCallback progress)
{
    return d->submit<PutObjectResult>(
        context, std::move(callback),
        [params](S3Client* c, auto done, auto progress) {
            return c->putObject(params, std::move(done), std::move(progress));
        },
        params.file.has_value(), std::move(progress));
}

RequestId Session::getObjectToFile(const GetObjectToFileParams& params, QObject* context,
                                   Callback<DownloadResult> callback, ProgressCallback progress)
{
    return d->submit<DownloadResult>(
        context, std::move(callback),
        [params](S3Client* c, auto done, auto progress) {
            return c->getObjectToFile(params, std::move(done), std::move(progress));
        },
        true, std::move(progress));
}

RequestId Session::deleteObject(const QString& bucket, const QString& key, const QString& versionId,
                                QObject* context, Callback<DeleteObjectResult> callback)
{
    return d->submit<DeleteObjectResult>(context, std::move(callback),
                                         [bucket, key, versionId](S3Client* c, auto done, auto /*progress*/) {
                                             return c->deleteObject(bucket, key, versionId, std::move(done));
                                         });
}

RequestId Session::deleteObjects(const QString& bucket, const QList<ObjectIdentifier>& objects,
                                 QObject* context, Callback<DeleteObjectsResult> callback)
{
    return d->submit<DeleteObjectsResult>(context, std::move(callback),
                                          [bucket, objects](S3Client* c, auto done, auto /*progress*/) {
                                              return c->deleteObjects(bucket, objects, std::move(done));
                                          });
}

RequestId Session::copyObject(const CopyObjectParams& params, QObject* context, Callback<CopyObjectResult> callback)
{
    return d->submit<CopyObjectResult>(context, std::move(callback), [params](S3Client* c, auto done, auto /*progress*/) {
        return c->copyObject(params, std::move(done));
    });
}

RequestId Session::createMultipartUpload(const CreateMultipartUploadParams& params, QObject* context,
                                         Callback<CreateMultipartUploadResult> callback)
{
    return d->submit<CreateMultipartUploadResult>(context, std::move(callback),
                                                  [params](S3Client* c, auto done, auto /*progress*/) {
                                                      return c->createMultipartUpload(params, std::move(done));
                                                  });
}

RequestId Session::uploadPart(const UploadPartParams& params, QObject* context, Callback<UploadPartResult> callback,
                              ProgressCallback progress)
{
    return d->submit<UploadPartResult>(
        context, std::move(callback),
        [params](S3Client* c, auto done, auto progress) {
            return c->uploadPart(params, std::move(done), std::move(progress));
        },
        true, std::move(progress));
}

RequestId Session::completeMultipartUpload(const CompleteMultipartUploadParams& params, QObject* context,
                                           Callback<CompleteMultipartUploadResult> callback)
{
    return d->submit<CompleteMultipartUploadResult>(context, std::move(callback),
                                                    [params](S3Client* c, auto done, auto /*progress*/) {
                                                        return c->completeMultipartUpload(params, std::move(done));
                                                    });
}

RequestId Session::abortMultipartUpload(const QString& bucket, const QString& key, const QString& uploadId,
                                        QObject* context, Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback),
                           [bucket, key, uploadId](S3Client* c, auto done, auto /*progress*/) {
                               return c->abortMultipartUpload(bucket, key, uploadId, std::move(done));
                           });
}

RequestId Session::listParts(const ListPartsParams& params, QObject* context, Callback<ListPartsPage> callback)
{
    return d->submit<ListPartsPage>(context, std::move(callback), [params](S3Client* c, auto done, auto /*progress*/) {
        return c->listParts(params, std::move(done));
    });
}

RequestId Session::listMultipartUploads(const ListMultipartUploadsParams& params, QObject* context,
                                        Callback<ListMultipartUploadsPage> callback)
{
    return d->submit<ListMultipartUploadsPage>(context, std::move(callback),
                                               [params](S3Client* c, auto done, auto /*progress*/) {
                                                   return c->listMultipartUploads(params, std::move(done));
                                               });
}

RequestId Session::uploadPartCopy(const UploadPartCopyParams& params, QObject* context,
                                  Callback<UploadPartCopyResult> callback)
{
    return d->submit<UploadPartCopyResult>(
        context, std::move(callback),
        [params](S3Client* c, auto done, auto /*progress*/) { return c->uploadPartCopy(params, std::move(done)); },
        true);
}

RequestId Session::createBucket(const CreateBucketParams& params, QObject* context,
                                Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback), [params](S3Client* c, auto done, auto /*progress*/) {
        return c->createBucket(params, std::move(done));
    });
}

RequestId Session::deleteBucket(const QString& bucket, QObject* context, Callback<void> callback)
{
    return d->submit<void>(context, std::move(callback), [bucket](S3Client* c, auto done, auto /*progress*/) {
        return c->deleteBucket(bucket, std::move(done));
    });
}

RequestId Session::probeServer(QObject* context, Callback<CapabilitySet> callback)
{
    return d->submit<CapabilitySet>(context, std::move(callback), [](S3Client* c, auto done, auto /*progress*/) {
        return c->probeServer(std::move(done));
    });
}

RequestId Session::probeBucket(const QString& bucket, QObject* context, Callback<CapabilitySet> callback)
{
    return probeBucket(bucket, QString(), context, std::move(callback));
}

RequestId Session::probeBucket(const QString& bucket, const QString& objectKey, QObject* context,
                               Callback<CapabilitySet> callback)
{
    return d->submit<CapabilitySet>(context, std::move(callback), [bucket, objectKey](S3Client* c, auto done, auto /*progress*/) {
        return c->probeBucket(bucket, objectKey, std::move(done));
    });
}

void Session::setCapabilities(const CapabilitySet& set)
{
    QMutexLocker lock(&d->mutex);
    d->capabilities.insert(set.scope == CapabilityScope::Server ? QString() : set.bucket, set);
    for (const auto& lane : d->lanes) {
        QMetaObject::invokeMethod(lane->client, [c = lane->client, set] { c->setCapabilities(set); },
                                  Qt::QueuedConnection);
    }
}

RequestId Session::presign(const PresignRequest& request, QObject* context, Callback<QUrl> callback)
{
    return d->submit<QUrl>(context, std::move(callback), [request](S3Client* c, auto done, auto /*progress*/) {
        return c->presign(request, std::move(done));
    });
}

} // namespace qbucket::s3
