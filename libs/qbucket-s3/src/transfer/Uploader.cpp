#include <qbucket/s3/transfer/Uploader.h>

#include <qbucket/s3/Logging.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Hasher.h>

#include "transfer/Async.h"
#include "transfer/Common.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace qbucket::s3::transfer {

namespace {

constexpr int kMaxHashing = 2;        // parallel hash tasks per upload

using detail::FileStamp;
using detail::isThrottle;
using detail::kPartRetries;
using detail::kRetryDelaysMs;
using detail::localError;
using detail::retryable;

} // namespace

struct Uploader::Impl {
    enum class PartStatus : quint8 { Pending, Hashing, Hashed, Uploading, Done };

    struct Part {
        PartStatus status = PartStatus::Pending;
        QByteArray md5;
        QByteArray sha256;
        QString etag;
        QString serverEtag;           // resume: the server has this part — ours?
        qint64 sent = 0;              // bytes of the current attempt
        int retries = 0;
        RequestId request = RequestId::Invalid;
        CancelFlag hash;
        std::optional<AdaptiveConcurrency::Slot> slot;
        bool verified = false;        // part ETag == part MD5
    };

    Uploader* q = nullptr;
    Session* session = nullptr;
    QPointer<AdaptiveConcurrency> limiter;
    std::unique_ptr<AdaptiveConcurrency> ownLimiter;
    std::shared_ptr<ResumeStore> store;
    UploadRequest req;

    State state = State::Idle;
    bool fromStore = false;
    bool resumed = false;
    bool restarted = false;
    PartPlan plan;
    QString uploadId;
    qint64 fileSize = 0;
    QDateTime fileModified;
    std::vector<Part> parts;
    qint64 doneBytes = 0;
    int doneParts = 0;
    int hashing = 0;
    int uploading = 0;
    bool completing = false;
    bool completeRetried = false;
    std::vector<RequestId> pendingRequests;   // create / complete / head / list
    std::vector<QPointer<QTimer>> retryTimers;
    QElapsedTimer progressTimer;
    Result<UploadResult> result = Error::make(ErrorCategory::Internal, QStringLiteral("upload not finished"));
    bool done = false;

    [[nodiscard]] bool persistent() const { return store != nullptr && plan.multipart; }

    TransferRecord record() const
    {
        TransferRecord r;
        r.id = req.transferId;
        r.direction = Direction::Upload;
        r.account = req.account;
        r.bucket = req.bucket;
        r.key = req.key;
        r.uploadId = uploadId;
        r.localPath = req.localPath;
        r.size = fileSize;
        r.localModified = fileModified;
        r.partSize = plan.partSize;
        r.partCount = plan.partCount;
        r.contentType = req.contentType;
        r.storageClass = req.storageClass;
        r.metadata = req.metadata;
        r.ifNoneMatch = req.ifNoneMatch;
        r.extra = req.extra;
        return r;
    }

    // --- resume store, in order on its own thread --------------------------------

    void storeWrite(std::function<Result<void>(ResumeStore&)> write)
    {
        if (!persistent())
            return;
        detail::serialStorePool()->start([s = store, write = std::move(write), id = req.transferId] {
            if (const Result<void> r = write(*s); !r)
                qCWarning(lcTransfer).noquote() << "resume store" << id << r.error().toString();
        });
    }

    void storeRemove()
    {
        if (!store || req.transferId.isEmpty())
            return;
        detail::serialStorePool()->start([s = store, id = req.transferId] { (void)s->remove(id); });
    }

    // --- finishing ----------------------------------------------------------------

    void stopEverything()
    {
        for (Part& p : parts) {
            if (p.hash)
                p.hash->store(true);
            if (p.request != RequestId::Invalid)
                session->cancel(p.request);   // the callback releases the slot
        }
        for (const RequestId id : pendingRequests)
            session->cancel(id);
        pendingRequests.clear();
        for (const QPointer<QTimer>& t : retryTimers) {
            if (t)
                t->deleteLater();
        }
        retryTimers.clear();
    }

    void finish(Result<UploadResult> r)
    {
        if (done)
            return;
        done = true;
        stopEverything();
        result = std::move(r);
        state = State::Finished;
        if (result)
            qCInfo(lcTransfer).noquote() << "uploaded" << req.bucket << req.key << result->size << "bytes";
        else
            qCInfo(lcTransfer).noquote() << "upload of" << req.key << "failed:" << result.error().toString();
        QMetaObject::invokeMethod(q, &Uploader::finished, Qt::QueuedConnection);
    }

    // keepForResume: leave the multipart upload and its record for a later
    // resume (transient trouble); otherwise abort it on the server.
    void fail(Error e, bool keepForResume)
    {
        if (done)
            return;
        if (!keepForResume)
            discardUpload();
        finish(std::move(e));
    }

    void discardUpload()
    {
        if (!uploadId.isEmpty()) {
            // Fire and forget: the request runs on even if we are gone.
            session->abortMultipartUpload(req.bucket, req.key, uploadId, q, [key = req.key](const Result<void>& r) {
                if (!r && r.error().category != ErrorCategory::NotFound)
                    qCWarning(lcTransfer).noquote() << "abort of the upload for" << key << "failed:" << r.error().toString();
            });
        }
        storeRemove();
    }

    // --- progress -------------------------------------------------------------------

    qint64 bytesDone() const
    {
        qint64 n = doneBytes;
        for (const Part& p : parts) {
            if (p.status == PartStatus::Uploading)
                n += p.sent;
        }
        return n;
    }

    void emitProgress(bool force)
    {
        if (!force && progressTimer.isValid() && progressTimer.elapsed() < 100)
            return;
        progressTimer.start();
        Q_EMIT q->progress(bytesDone(), fileSize);
    }

    // --- the start ----------------------------------------------------------------

    void begin()
    {
        const FileStamp stamp = FileStamp::of(req.localPath);
        if (!stamp.exists) {
            fail(localError(QStringLiteral("%1: no such file").arg(req.localPath)), false);
            return;
        }
        fileSize = stamp.size;
        fileModified = stamp.modified;
        Result<PartPlan> p = planParts(fileSize, *session->options().profile, req.planner);
        if (!p) {
            fail(p.error(), false);
            return;
        }
        plan = *p;
        if (plan.multipart && store && !isValidTransferId(req.transferId)) {
            fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("a resumable upload needs a valid transfer id")),
                 false);
            return;
        }
        parts.assign(size_t(plan.partCount), Part());
        emitProgress(true);
        if (!plan.multipart) {
            pump();
            return;
        }
        createUpload();
    }

    void createUpload()
    {
        CreateMultipartUploadParams p;
        p.bucket = req.bucket;
        p.key = req.key;
        p.contentType = req.contentType;
        p.metadata = req.metadata;
        p.storageClass = req.storageClass;
        const RequestId id = session->createMultipartUpload(p, q, [this](Result<CreateMultipartUploadResult> r) {
            if (done)
                return;
            if (!r) {
                fail(r.error(), false);
                return;
            }
            uploadId = r->uploadId;
            storeWrite([rec = record()](ResumeStore& s) { return s.begin(rec); });
            pump();
        });
        pendingRequests.push_back(id);
    }

    // Continues a stored upload: the record says what was planned, ListParts
    // what the server really has.
    void resumeStored()
    {
        auto s = store;
        const QString id = req.transferId;
        detail::runAsync<Result<ResumeState>>(
            detail::serialStorePool(), q, [s, id] { return s->load(id); },
            [this](Result<ResumeState> loaded) {
                if (done)
                    return;
                if (!loaded) {
                    fail(loaded.error(), true);
                    return;
                }
                adoptRecord(*loaded);
            });
    }

    void adoptRecord(const ResumeState& state_)
    {
        const TransferRecord& r = state_.record;
        if (r.direction != Direction::Upload || r.partCount < 1 || r.partSize <= 0 || r.uploadId.isEmpty()) {
            fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("not a resumable upload record")), false);
            return;
        }
        req.bucket = r.bucket;
        req.key = r.key;
        req.localPath = r.localPath;
        req.account = r.account;
        req.contentType = r.contentType;
        req.storageClass = r.storageClass;
        req.metadata = r.metadata;
        req.ifNoneMatch = r.ifNoneMatch;
        req.extra = r.extra;
        fileSize = r.size;
        fileModified = r.localModified;
        uploadId = r.uploadId;
        plan.size = r.size;
        plan.multipart = true;
        plan.partSize = r.partSize;
        plan.partCount = r.partCount;
        if (plan.partLength(plan.partCount) <= 0 || plan.partLength(plan.partCount) > plan.partSize) {
            fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("inconsistent upload record")), false);
            return;
        }
        if (!FileStamp::of(req.localPath).sameAs(fileSize, fileModified)) {
            // Different bytes than the parts already sent: they are useless.
            fail(localError(QStringLiteral("%1 changed since its upload started; start it again").arg(req.localPath)),
                 false);
            return;
        }
        parts.assign(size_t(plan.partCount), Part());
        std::map<int, PartRecord> recorded;
        for (const PartRecord& p : state_.parts)
            recorded[p.partNumber] = p;
        resumed = true;
        listServerParts(0, std::move(recorded), {});
    }

    void listServerParts(int marker, std::map<int, PartRecord> recorded, std::map<int, PartInfo> server)
    {
        ListPartsParams p;
        p.bucket = req.bucket;
        p.key = req.key;
        p.uploadId = uploadId;
        p.partNumberMarker = marker;
        const RequestId id = session->listParts(
            p, q, [this, recorded = std::move(recorded), server = std::move(server)](Result<ListPartsPage> page) mutable {
                if (done)
                    return;
                if (!page) {
                    if (page.error().category == ErrorCategory::NotFound) {
                        // Aborted or expired: start over with a new upload.
                        qCInfo(lcTransfer).noquote() << "upload of" << req.key << "is gone on the server; starting over";
                        restarted = true;
                        uploadId.clear();
                        createUpload();
                        return;
                    }
                    fail(page.error(), true);
                    return;
                }
                for (const PartInfo& i : page->parts)
                    server[i.partNumber] = i;
                if (page->isTruncated) {
                    listServerParts(page->nextPartNumberMarker, std::move(recorded), std::move(server));
                    return;
                }
                matchParts(recorded, server);
            });
        pendingRequests.push_back(id);
    }

    void matchParts(const std::map<int, PartRecord>& recorded, const std::map<int, PartInfo>& server)
    {
        for (int n = 1; n <= plan.partCount; ++n) {
            Part& part = parts[size_t(n - 1)];
            const auto s = server.find(n);
            if (s == server.end() || s->second.size != plan.partLength(n))
                continue;   // not there (or not ours): upload it
            const auto r = recorded.find(n);
            if (r != recorded.end() && r->second.length == plan.partLength(n) && sameEtag(r->second.etag, s->second.etag)) {
                part.status = PartStatus::Done;
                part.md5 = r->second.md5;
                part.etag = s->second.etag;
                part.verified = sameEtag(part.etag, md5Etag(part.md5));
                doneBytes += plan.partLength(n);
                ++doneParts;
            } else {
                // On the server but not recorded (the process died in
                // between): ours if the local bytes hash to its ETag.
                part.serverEtag = s->second.etag;
            }
        }
        qCInfo(lcTransfer).noquote() << "resuming upload of" << req.key << ":" << doneParts << "of" << plan.partCount
                                    << "parts already there";
        emitProgress(true);
        pump();
    }

    // --- the work loop ----------------------------------------------------------

    void pump()
    {
        if (done || state == State::Idle)
            return;
        if (state == State::Pausing || state == State::Paused) {
            if (state == State::Pausing && uploading == 0 && hashing == 0) {
                state = State::Paused;
                Q_EMIT q->paused();
            }
            return;
        }
        if (plan.multipart && uploadId.isEmpty())
            return;   // still creating
        if (doneParts == plan.partCount) {
            if (!completing)
                completeUpload();
            return;
        }
        // Hash ahead: at most kMaxHashing at once, not much further ahead
        // than the uploads can follow.
        int hashed = 0;
        for (const Part& p : parts)
            hashed += p.status == PartStatus::Hashed ? 1 : 0;
        const int lookahead = (limiter ? limiter->limit() : 4) + 1;
        for (size_t i = 0; i < parts.size() && hashing < kMaxHashing && hashed + hashing < lookahead; ++i) {
            if (parts[i].status == PartStatus::Pending)
                startHash(int(i) + 1);
        }
        // Send what is hashed, as far as the account's limit allows.
        for (size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].status != PartStatus::Hashed)
                continue;
            const std::optional<AdaptiveConcurrency::Slot> slot = limiter->tryAcquire();
            if (!slot)
                break;
            sendPart(int(i) + 1, *slot);
        }
    }

    void startHash(int n)
    {
        Part& p = parts[size_t(n - 1)];
        p.status = PartStatus::Hashing;
        ++hashing;
        const bool sha = session->requiresPayloadHash();
        p.hash = hashRange(req.localPath, plan.partOffset(n), plan.partLength(n), sha, q,
                           [this, n](Result<RangeHash> h) {
                               --hashing;
                               if (done)
                                   return;
                               Part& part = parts[size_t(n - 1)];
                               part.hash.reset();
                               if (!h) {
                                   fail(h.error(), h.error().category != ErrorCategory::LocalIo);
                                   return;
                               }
                               part.md5 = h->md5;
                               part.sha256 = h->sha256;
                               if (!part.serverEtag.isEmpty() && sameEtag(part.serverEtag, md5Etag(part.md5))) {
                                   // Uploaded before the crash: keep it.
                                   part.status = PartStatus::Done;
                                   part.etag = part.serverEtag;
                                   part.verified = true;
                                   recordPart(n);
                                   doneBytes += plan.partLength(n);
                                   ++doneParts;
                                   emitProgress(true);
                               } else {
                                   part.status = PartStatus::Hashed;
                               }
                               pump();
                           });
    }

    FileBody bodyOf(int n) const
    {
        const Part& p = parts[size_t(n - 1)];
        FileBody b;
        b.path = req.localPath;
        b.offset = plan.partOffset(n);
        b.length = plan.partLength(n);
        b.md5Base64 = p.md5.toBase64();
        if (!p.sha256.isEmpty())
            b.sha256Hex = p.sha256.toHex();
        return b;
    }

    void sendPart(int n, AdaptiveConcurrency::Slot slot)
    {
        Part& p = parts[size_t(n - 1)];
        p.status = PartStatus::Uploading;
        p.slot = slot;
        p.sent = 0;
        ++uploading;
        const auto onProgress = [this, n](qint64 sent, qint64) {
            parts[size_t(n - 1)].sent = sent;
            emitProgress(false);
        };
        if (!plan.multipart) {
            PutObjectParams put;
            put.bucket = req.bucket;
            put.key = req.key;
            put.file = bodyOf(n);
            put.contentType = req.contentType;
            put.metadata = req.metadata;
            put.storageClass = req.storageClass;
            put.ifNoneMatch = req.ifNoneMatch;
            p.request = session->putObject(put, q, [this](Result<PutObjectResult> r) {
                const Error* e = r ? nullptr : &r.error();
                if (!partFinished(1, e, r && r->throttled))
                    return;
                if (!r) {
                    partFailed(1, r.error());
                    return;
                }
                const bool verified = sameEtag(r->etag, md5Etag(parts[0].md5));
                verifyAndFinish(r->etag, r->versionId, r->requestId, verified);
            }, onProgress);
            return;
        }
        UploadPartParams up;
        up.bucket = req.bucket;
        up.key = req.key;
        up.uploadId = uploadId;
        up.partNumber = n;
        up.file = bodyOf(n);
        p.request = session->uploadPart(up, q, [this, n](Result<UploadPartResult> r) {
            const Error* e = r ? nullptr : &r.error();
            if (!partFinished(n, e, r && r->throttled))
                return;
            if (!r) {
                partFailed(n, r.error());
                return;
            }
            Part& part = parts[size_t(n - 1)];
            part.status = PartStatus::Done;
            part.etag = r->etag;
            // The server checked Content-MD5 already; an ETag of another
            // kind (SSE-C, KMS) only means we can't confirm it again.
            part.verified = sameEtag(r->etag, md5Etag(part.md5));
            recordPart(n);
            doneBytes += plan.partLength(n);
            ++doneParts;
            emitProgress(true);
            pump();
        }, onProgress);
    }

    // Bookkeeping common to every part answer; false: nothing more to do.
    bool partFinished(int n, const Error* error, bool throttled)
    {
        Part& p = parts[size_t(n - 1)];
        p.request = RequestId::Invalid;
        --uploading;
        if (p.slot && limiter) {
            using O = AdaptiveConcurrency::Outcome;
            const O outcome = (throttled || (error && isThrottle(*error))) ? O::Throttled
                            : error ? O::Failed
                                    : O::Succeeded;
            limiter->release(*p.slot, outcome);
        }
        p.slot.reset();
        p.sent = 0;
        if (done)
            return false;
        if (error && error->category == ErrorCategory::Cancelled && (state == State::Pausing || state == State::Paused)) {
            p.status = PartStatus::Hashed;
            pump();
            return false;
        }
        return true;
    }

    void partFailed(int n, const Error& e)
    {
        Part& p = parts[size_t(n - 1)];
        if (e.code == QLatin1String("BadDigest")) {
            // The bytes on disk no longer match the hash: the file changes.
            fail(localError(QStringLiteral("%1 changed while it was being uploaded").arg(req.localPath)), false);
            return;
        }
        if (e.category == ErrorCategory::LocalIo) {
            fail(e, false);
            return;
        }
        if (retryable(e) && p.retries < kPartRetries) {
            const int delay = kRetryDelaysMs[p.retries++];
            qCInfo(lcTransfer).noquote() << "part" << n << "of" << req.key << "failed (" << e.toString() << "); again in"
                                        << delay << "ms";
            p.status = PartStatus::Hashing;   // parked until the timer fires
            auto* timer = new QTimer(q);
            timer->setSingleShot(true);
            QObject::connect(timer, &QTimer::timeout, q, [this, n, timer] {
                timer->deleteLater();
                if (done)
                    return;
                parts[size_t(n - 1)].status = PartStatus::Hashed;
                pump();
            });
            retryTimers.push_back(timer);
            timer->start(delay);
            return;
        }
        // Permanent, or retries used up: keep what is uploaded for later.
        fail(e, plan.multipart && retryable(e));
    }

    void recordPart(int n)
    {
        const Part& p = parts[size_t(n - 1)];
        PartRecord r;
        r.partNumber = n;
        r.offset = plan.partOffset(n);
        r.length = plan.partLength(n);
        r.etag = p.etag;
        r.md5 = p.md5;
        storeWrite([id = req.transferId, r](ResumeStore& s) { return s.addPart(id, r); });
    }

    // --- complete -----------------------------------------------------------------

    QString expectedEtag() const
    {
        QList<QByteArray> md5s;
        md5s.reserve(int(parts.size()));
        for (const Part& p : parts)
            md5s.append(p.md5);
        return compositeEtag(md5s);
    }

    bool allPartsVerified() const
    {
        return std::all_of(parts.begin(), parts.end(), [](const Part& p) { return p.verified; });
    }

    void completeUpload()
    {
        completing = true;
        CompleteMultipartUploadParams p;
        p.bucket = req.bucket;
        p.key = req.key;
        p.uploadId = uploadId;
        p.ifNoneMatch = req.ifNoneMatch;
        for (size_t i = 0; i < parts.size(); ++i)
            p.parts.append({int(i) + 1, parts[i].etag});
        const RequestId id = session->completeMultipartUpload(p, q, [this](Result<CompleteMultipartUploadResult> r) {
            if (done)
                return;
            if (r) {
                verifyAndFinish(r->etag, r->versionId, r->requestId,
                                allPartsVerified() && sameEtag(r->etag, expectedEtag()));
                return;
            }
            const Error& e = r.error();
            if (e.outcomeUnknown || e.category == ErrorCategory::NotFound || e.isTransient()) {
                resolveComplete(e);
                return;
            }
            // PreconditionFailed (If-None-Match), InvalidPart, …: the parts
            // can't become this object.
            fail(e, false);
        });
        pendingRequests.push_back(id);
    }

    // Did the lost / failed Complete land? HEAD decides; while the upload
    // still exists, Complete is tried once more.
    void resolveComplete(const Error& original)
    {
        const RequestId id = session->headObject(req.bucket, req.key, {}, q, [this, original](Result<ObjectMeta> head) {
            if (done)
                return;
            if (head && head->size == fileSize && sameEtag(head->etag, expectedEtag())) {
                verifyAndFinish(head->etag, head->versionId, {}, allPartsVerified());
                return;
            }
            ListPartsParams lp;
            lp.bucket = req.bucket;
            lp.key = req.key;
            lp.uploadId = uploadId;
            lp.maxParts = 1;
            const RequestId listId = session->listParts(lp, q, [this, original, head](const Result<ListPartsPage>& page) {
                if (done)
                    return;
                if (page && !completeRetried) {
                    completeRetried = true;   // still open: it didn't land
                    completeUpload();
                    return;
                }
                if (!page && page.error().category == ErrorCategory::NotFound && head && head->size == fileSize
                    && etagPartCount(head->etag) == plan.partCount) {
                    // Completed (ETag of another kind than MD5-of-MD5s).
                    verifyAndFinish(head->etag, head->versionId, {}, false);
                    return;
                }
                Error e = original;
                e.message = QStringLiteral("could not tell whether the upload completed: ") + e.message;
                fail(e, true);
            });
            pendingRequests.push_back(listId);
        });
        pendingRequests.push_back(id);
    }

    // The object is there; check it and the source once more.
    void verifyAndFinish(const QString& etag, const QString& versionId, const QString& requestId, bool md5Verified)
    {
        if (!FileStamp::of(req.localPath).sameAs(fileSize, fileModified)) {
            fail(localError(QStringLiteral("%1 changed while it was being uploaded; the object holds the earlier "
                                           "content").arg(req.localPath)),
                 false);
            return;
        }
        UploadResult out;
        out.etag = etag;
        out.versionId = versionId;
        out.requestId = requestId;
        out.uploadId = uploadId;
        out.size = fileSize;
        out.parts = plan.partCount;
        out.multipart = plan.multipart;
        out.resumed = resumed;
        out.restarted = restarted;
        if (md5Verified) {
            out.integrity = Integrity::Md5Verified;
            storeRemove();
            finish(out);
            return;
        }
        // An ETag we can't recompute: at least the size must be right.
        const RequestId id = session->headObject(req.bucket, req.key, versionId, q, [this, out](Result<ObjectMeta> head) mutable {
            if (done)
                return;
            if (!head) {
                fail(head.error(), true);
                return;
            }
            if (head->size != fileSize) {
                fail(Error::make(ErrorCategory::Protocol, QStringLiteral("the uploaded object has %1 bytes, expected %2")
                                                              .arg(head->size)
                                                              .arg(fileSize)),
                     false);
                return;
            }
            out.integrity = Integrity::SizeVerified;
            storeRemove();
            finish(out);
        });
        pendingRequests.push_back(id);
    }
};

Uploader::Uploader(Session* session, UploadRequest request, AdaptiveConcurrency* limiter,
                   std::shared_ptr<ResumeStore> store, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    Q_ASSERT(session);
    d->q = this;
    d->session = session;
    d->store = std::move(store);
    d->req = std::move(request);
    if (limiter) {
        d->limiter = limiter;
    } else {
        d->ownLimiter = std::make_unique<AdaptiveConcurrency>();
        d->limiter = d->ownLimiter.get();
    }
    connect(d->limiter, &AdaptiveConcurrency::slotAvailable, this, [this] { d->pump(); });
}

Uploader::~Uploader()
{
    // Requests still out deliver nothing to us any more: give their slots back.
    for (Impl::Part& p : d->parts) {
        if (p.slot && d->limiter)
            d->limiter->release(*p.slot, AdaptiveConcurrency::Outcome::Failed);
        p.slot.reset();
        if (p.hash)
            p.hash->store(true);
        if (p.request != RequestId::Invalid)
            d->session->cancel(p.request);
    }
    for (const RequestId id : d->pendingRequests)
        d->session->cancel(id);
}

std::unique_ptr<Uploader> Uploader::fromStore(Session* session, std::shared_ptr<ResumeStore> store,
                                              const QString& transferId, AdaptiveConcurrency* limiter, QObject* parent)
{
    UploadRequest r;
    r.transferId = transferId;
    auto up = std::make_unique<Uploader>(session, r, limiter, std::move(store), parent);
    up->d->fromStore = true;
    return up;
}

const UploadRequest& Uploader::request() const
{
    return d->req;
}

Uploader::State Uploader::state() const
{
    return d->state;
}

qint64 Uploader::bytesDone() const
{
    return d->bytesDone();
}

qint64 Uploader::bytesTotal() const
{
    return d->fileSize;
}

int Uploader::partsDone() const
{
    return d->doneParts;
}

int Uploader::partCount() const
{
    return d->plan.partCount;
}

const Result<UploadResult>& Uploader::result() const
{
    return d->result;
}

void Uploader::start()
{
    if (d->state != State::Idle)
        return;
    d->state = State::Running;
    if (d->fromStore) {
        if (!d->store) {
            d->fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("no resume store")), true);
            return;
        }
        d->resumeStored();
        return;
    }
    d->begin();
}

void Uploader::pause()
{
    if (d->state != State::Running)
        return;
    d->state = State::Pausing;
    d->pump();
}

void Uploader::resume()
{
    if (d->state != State::Pausing && d->state != State::Paused)
        return;
    d->state = State::Running;
    d->pump();
}

void Uploader::cancel(bool keepForResume)
{
    if (d->done)
        return;
    Error e = Error::make(ErrorCategory::Cancelled, QStringLiteral("upload cancelled"));
    if (keepForResume)
        d->finish(e);
    else
        d->fail(e, false);
}

} // namespace qbucket::s3::transfer
