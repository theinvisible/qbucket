#include <qbucket/s3/transfer/Copier.h>

#include <qbucket/s3/Logging.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Hasher.h>
#include <qbucket/s3/transfer/PartPlanner.h>

#include "transfer/Common.h"

#include <QPointer>
#include <QTimer>

#include <optional>
#include <vector>

namespace qbucket::s3::transfer {

namespace {

using detail::isThrottle;
using detail::kPartRetries;
using detail::kRetryDelaysMs;
using detail::retryable;

constexpr qint64 kMiB = Q_INT64_C(1024) * 1024;

} // namespace

struct Copier::Impl {
    enum class PartStatus : quint8 { Pending, Copying, Done };

    struct Part {
        PartStatus status = PartStatus::Pending;
        QString etag;
        int retries = 0;
        bool retryPending = false;
        RequestId request = RequestId::Invalid;
        std::optional<AdaptiveConcurrency::Slot> slot;
    };

    Copier* q = nullptr;
    Session* session = nullptr;
    QPointer<AdaptiveConcurrency> limiter;
    std::unique_ptr<AdaptiveConcurrency> ownLimiter;
    CopyRequest req;

    bool started = false;
    ObjectMeta source;
    PartPlan plan;
    QString uploadId;
    std::vector<Part> parts;
    qint64 doneBytes = 0;
    int doneParts = 0;
    bool completing = false;
    bool completeRetried = false;
    std::vector<RequestId> pendingRequests;
    Result<CopyResult> result = Error::make(ErrorCategory::Internal, QStringLiteral("copy not finished"));
    bool done = false;

    void finish(Result<CopyResult> r)
    {
        if (done)
            return;
        done = true;
        for (Part& p : parts) {
            if (p.request != RequestId::Invalid)
                session->cancel(p.request);
        }
        for (const RequestId id : pendingRequests)
            session->cancel(id);
        pendingRequests.clear();
        result = std::move(r);
        if (result)
            qCInfo(lcTransfer).noquote() << "copied" << req.sourceKey << "to" << req.key << result->size << "bytes";
        else
            qCInfo(lcTransfer).noquote() << "copy of" << req.sourceKey << "failed:" << result.error().toString();
        QMetaObject::invokeMethod(q, &Copier::finished, Qt::QueuedConnection);
    }

    void fail(Error e)
    {
        if (done)
            return;
        if (!uploadId.isEmpty()) {
            session->abortMultipartUpload(req.bucket, req.key, uploadId, q, [key = req.key](const Result<void>& r) {
                if (!r && r.error().category != ErrorCategory::NotFound)
                    qCWarning(lcTransfer).noquote() << "abort of the copy to" << key << "failed:" << r.error().toString();
            });
        }
        finish(std::move(e));
    }

    void begin()
    {
        HeadObjectParams h;
        h.bucket = req.sourceBucket;
        h.key = req.sourceKey;
        h.versionId = req.sourceVersionId;
        h.ifMatch = req.ifMatch;
        const RequestId id = session->headObject(h, q, [this](Result<ObjectMeta> head) {
            if (done)
                return;
            if (!head) {
                fail(head.error());
                return;
            }
            source = *head;
            if (source.size < 0) {
                fail(Error::make(ErrorCategory::Protocol, QStringLiteral("the server reported no size for the source")));
                return;
            }
            if (req.sourceVersionId.isEmpty() && !source.versionId.isEmpty() && source.versionId != QLatin1String("null"))
                req.sourceVersionId = source.versionId;   // copy exactly what we looked at
            const ProviderProfile& profile = *session->options().profile;
            qint64 partSize = std::max(req.partSize, profile.minPartSize());
            const qint64 needed = (source.size + profile.maxParts() - 1) / profile.maxParts();
            if (needed > partSize)
                partSize = (needed + kMiB - 1) / kMiB * kMiB;
            if (partSize > profile.maxPartSize() || source.size > kMaxObjectSize) {
                fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("the object is too large to copy")));
                return;
            }
            plan.size = source.size;
            plan.multipart = true;
            plan.partSize = partSize;
            plan.partCount = source.size == 0 ? 1 : int((source.size + partSize - 1) / partSize);
            parts.assign(size_t(plan.partCount), Part());
            create();
        });
        pendingRequests.push_back(id);
    }

    void create()
    {
        CreateMultipartUploadParams p;
        p.bucket = req.bucket;
        p.key = req.key;
        // Everything a client can set travels along (COPY semantics).
        p.headers = objectHeadersOf(source);
        p.storageClass = req.storageClass.isEmpty() ? source.storageClass : req.storageClass;
        const RequestId id = session->createMultipartUpload(p, q, [this](Result<CreateMultipartUploadResult> r) {
            if (done)
                return;
            if (!r) {
                fail(r.error());
                return;
            }
            uploadId = r->uploadId;
            pump();
        });
        pendingRequests.push_back(id);
    }

    void pump()
    {
        if (done || uploadId.isEmpty())
            return;
        if (doneParts == plan.partCount) {
            if (!completing)
                complete();
            return;
        }
        for (size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].status != PartStatus::Pending || parts[i].retryPending)
                continue;
            const std::optional<AdaptiveConcurrency::Slot> slot = limiter->tryAcquire();
            if (!slot)
                break;
            copyPart(int(i) + 1, *slot);
        }
    }

    void copyPart(int n, AdaptiveConcurrency::Slot slot)
    {
        Part& p = parts[size_t(n - 1)];
        p.status = PartStatus::Copying;
        p.slot = slot;
        UploadPartCopyParams c;
        c.sourceBucket = req.sourceBucket;
        c.sourceKey = req.sourceKey;
        c.sourceVersionId = req.sourceVersionId;
        c.bucket = req.bucket;
        c.key = req.key;
        c.uploadId = uploadId;
        c.partNumber = n;
        if (plan.size > 0) {
            c.rangeStart = plan.partOffset(n);
            c.rangeLength = plan.partLength(n);
        }
        c.ifMatch = source.etag;
        p.request = session->uploadPartCopy(c, q, [this, n](Result<UploadPartCopyResult> r) {
            Part& part = parts[size_t(n - 1)];
            part.request = RequestId::Invalid;
            if (part.slot && limiter) {
                using O = AdaptiveConcurrency::Outcome;
                const O outcome = (r && r->throttled) || (!r && isThrottle(r.error())) ? O::Throttled
                                : r ? O::Succeeded
                                    : O::Failed;
                limiter->release(*part.slot, outcome);
            }
            part.slot.reset();
            if (done)
                return;
            if (!r) {
                partFailed(n, r.error());
                return;
            }
            part.status = PartStatus::Done;
            part.etag = r->etag;
            doneBytes += plan.partLength(n);
            ++doneParts;
            Q_EMIT q->progress(doneBytes, plan.size);
            pump();
        });
    }

    void partFailed(int n, const Error& e)
    {
        Part& p = parts[size_t(n - 1)];
        if (e.category == ErrorCategory::PreconditionFailed) {
            fail(Error::make(ErrorCategory::PreconditionFailed,
                             QStringLiteral("%1 changed during the copy").arg(req.sourceKey)));
            return;
        }
        if (retryable(e) && p.retries < kPartRetries) {
            const int delay = kRetryDelaysMs[p.retries++];
            qCInfo(lcTransfer).noquote() << "copy of part" << n << "failed (" << e.toString() << "); again in" << delay << "ms";
            p.status = PartStatus::Pending;
            p.retryPending = true;
            QTimer::singleShot(delay, q, [this, n] {
                if (done)
                    return;
                parts[size_t(n - 1)].retryPending = false;
                pump();
            });
            return;
        }
        fail(e);
    }

    void complete()
    {
        completing = true;
        CompleteMultipartUploadParams p;
        p.bucket = req.bucket;
        p.key = req.key;
        p.uploadId = uploadId;
        for (size_t i = 0; i < parts.size(); ++i)
            p.parts.append({int(i) + 1, parts[i].etag});
        const RequestId id = session->completeMultipartUpload(p, q, [this](Result<CompleteMultipartUploadResult> r) {
            if (done)
                return;
            if (r) {
                verify(r->etag, r->versionId);
                return;
            }
            const Error& e = r.error();
            if (e.outcomeUnknown || e.category == ErrorCategory::NotFound || e.isTransient()) {
                resolve(e);
                return;
            }
            fail(e);
        });
        pendingRequests.push_back(id);
    }

    QString expectedEtag() const
    {
        // The copy's parts are new uploads: MD5-of-the-part-ETags (as MD5s).
        QList<QByteArray> md5s;
        for (const Part& p : parts) {
            QString plain = p.etag;
            plain.remove(QLatin1Char('"'));
            md5s.append(QByteArray::fromHex(plain.toLatin1()));
        }
        return compositeEtag(md5s);
    }

    void resolve(const Error& original)
    {
        const RequestId id = session->headObject(req.bucket, req.key, {}, q, [this, original](Result<ObjectMeta> head) {
            if (done)
                return;
            if (head && head->size == plan.size && sameEtag(head->etag, expectedEtag())) {
                verify(head->etag, head->versionId);
                return;
            }
            if (!completeRetried) {
                ListPartsParams lp;
                lp.bucket = req.bucket;
                lp.key = req.key;
                lp.uploadId = uploadId;
                lp.maxParts = 1;
                const RequestId listId = session->listParts(lp, q, [this, original](const Result<ListPartsPage>& page) {
                    if (done)
                        return;
                    if (page) {
                        completeRetried = true;
                        complete();
                        return;
                    }
                    Error e = original;
                    e.message = QStringLiteral("could not tell whether the copy completed: ") + e.message;
                    finish(e);   // nothing to abort any more
                });
                pendingRequests.push_back(listId);
                return;
            }
            Error e = original;
            e.message = QStringLiteral("could not tell whether the copy completed: ") + e.message;
            finish(e);
        });
        pendingRequests.push_back(id);
    }

    void verify(const QString& etag, const QString& versionId)
    {
        const RequestId id = session->headObject(req.bucket, req.key, versionId, q, [this, etag, versionId](Result<ObjectMeta> head) {
            if (done)
                return;
            if (!head) {
                finish(head.error());
                return;
            }
            if (head->size != plan.size) {
                finish(Error::make(ErrorCategory::Protocol, QStringLiteral("the copy has %1 bytes, the source %2")
                                                                .arg(head->size)
                                                                .arg(plan.size)));
                return;
            }
            CopyResult out;
            out.etag = etag;
            out.versionId = versionId;
            out.sourceEtag = source.etag;
            out.size = plan.size;
            out.parts = plan.partCount;
            finish(out);
        });
        pendingRequests.push_back(id);
    }
};

Copier::Copier(Session* session, CopyRequest request, AdaptiveConcurrency* limiter, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    Q_ASSERT(session);
    d->q = this;
    d->session = session;
    d->req = std::move(request);
    if (limiter) {
        d->limiter = limiter;
    } else {
        d->ownLimiter = std::make_unique<AdaptiveConcurrency>();
        d->limiter = d->ownLimiter.get();
    }
    connect(d->limiter, &AdaptiveConcurrency::slotAvailable, this, [this] { d->pump(); });
}

Copier::~Copier()
{
    for (Impl::Part& p : d->parts) {
        if (p.slot && d->limiter)
            d->limiter->release(*p.slot, AdaptiveConcurrency::Outcome::Failed);
        p.slot.reset();
        if (p.request != RequestId::Invalid)
            d->session->cancel(p.request);
    }
    for (const RequestId id : d->pendingRequests)
        d->session->cancel(id);
}

const CopyRequest& Copier::request() const
{
    return d->req;
}

qint64 Copier::bytesDone() const
{
    return d->doneBytes;
}

qint64 Copier::bytesTotal() const
{
    return d->plan.size;
}

const Result<CopyResult>& Copier::result() const
{
    return d->result;
}

void Copier::start()
{
    if (d->started)
        return;
    d->started = true;
    d->begin();
}

void Copier::cancel()
{
    d->fail(Error::make(ErrorCategory::Cancelled, QStringLiteral("copy cancelled")));
}

} // namespace qbucket::s3::transfer
