#include <qbucket/s3/transfer/Downloader.h>

#include <qbucket/s3/Logging.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Hasher.h>

#include "transfer/Async.h"
#include "transfer/Common.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <system_error>
#include <vector>

namespace qbucket::s3::transfer {

namespace {

using detail::isThrottle;
using detail::kPartRetries;
using detail::kRetryDelaysMs;
using detail::localError;
using detail::retryable;

// Only then is an ETag the MD5 of the content: one part, no KMS / SSE-C.
bool etagIsContentMd5(const ObjectMeta& meta)
{
    QString plain = meta.etag;
    plain.remove(QLatin1Char('"'));
    if (plain.size() != 32 || plain.contains(QLatin1Char('-')))
        return false;
    for (const QChar c : plain) {
        if (!c.isDigit() && !(c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f')))
            return false;
    }
    for (const auto& [name, value] : meta.responseHeaders) {
        if (name.compare(QLatin1String("x-amz-server-side-encryption"), Qt::CaseInsensitive) == 0
            && value.startsWith(QLatin1String("aws:kms")))
            return false;
        if (name.compare(QLatin1String("x-amz-server-side-encryption-customer-algorithm"), Qt::CaseInsensitive) == 0)
            return false;
    }
    return true;
}

// Creates the parent directory and a file of `size` bytes (sparse where
// the file system can). fresh: truncate first.
Result<void> preparePartial(const QString& path, qint64 size, bool fresh)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return localError(QStringLiteral("cannot create the folder for %1").arg(path));
    QFile f(path);
    if (!f.open(fresh ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::ReadWrite))
        return localError(QStringLiteral("%1: %2").arg(path, f.errorString()));
    if (!f.resize(size))
        return localError(QStringLiteral("%1: %2").arg(path, f.errorString()));
    return {};
}

// Moves the verified partial file to its name; mtime from the object.
Result<void> finalizeFile(const QString& partial, const QString& target, bool overwrite, qint64 size,
                          const QDateTime& modified)
{
    const QFileInfo done(partial);
    if (!done.exists() || done.size() != size)
        return localError(QStringLiteral("%1 has %2 bytes, expected %3").arg(partial).arg(done.size()).arg(size));
    const QFileInfo existing(target);
    if (existing.exists()) {
        if (existing.isDir())
            return localError(QStringLiteral("%1 is a folder").arg(target));
        if (!overwrite)
            return Error::make(ErrorCategory::Conflict, QStringLiteral("%1 exists already").arg(target));
    }
    // Replaces the target in one step (rename(2), MoveFileEx).
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(partial.toStdU16String()),
                            std::filesystem::path(target.toStdU16String()), ec);
    if (ec)
        return localError(QStringLiteral("cannot rename %1 to %2: %3").arg(partial, target, QString::fromStdString(ec.message())));
    if (modified.isValid()) {
        QFile f(target);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(modified, QFileDevice::FileModificationTime);
    }
    return {};
}

} // namespace

struct Downloader::Impl {
    enum class PartStatus : quint8 { Pending, Checking, Fetching, Done };

    struct Part {
        PartStatus status = PartStatus::Pending;
        QByteArray md5;
        qint64 received = 0;
        int retries = 0;
        RequestId request = RequestId::Invalid;
        std::optional<AdaptiveConcurrency::Slot> slot;
        bool retryPending = false;
    };

    Downloader* q = nullptr;
    Session* session = nullptr;
    QPointer<AdaptiveConcurrency> limiter;
    std::unique_ptr<AdaptiveConcurrency> ownLimiter;
    std::shared_ptr<ResumeStore> store;
    DownloadRequest req;

    State state = State::Idle;
    bool fromStore = false;
    bool resumed = false;
    bool restarted = false;
    PartPlan plan;
    bool layoutFromObject = false;   // ranges = the object's own parts (uniform)
    bool md5Comparable = false;      // ETag is the content's MD5
    ObjectMeta meta;                 // HEAD of the object we download
    QString partial;
    std::vector<Part> parts;
    Integrity singleIntegrity = Integrity::SizeVerified;
    qint64 doneBytes = 0;
    int doneParts = 0;
    int fetching = 0;
    bool verifying = false;
    std::vector<RequestId> pendingRequests;
    QElapsedTimer progressTimer;
    Result<FileDownloadResult> result = Error::make(ErrorCategory::Internal, QStringLiteral("download not finished"));
    bool done = false;

    [[nodiscard]] bool persistent() const { return store != nullptr && isValidTransferId(req.transferId); }

    TransferRecord record() const
    {
        TransferRecord r;
        r.id = req.transferId;
        r.direction = Direction::Download;
        r.account = req.account;
        r.bucket = req.bucket;
        r.key = req.key;
        r.versionId = req.versionId;
        r.localPath = partial;
        r.finalPath = req.targetPath;
        r.size = plan.size;
        r.etag = meta.etag;
        r.partSize = plan.partSize;
        r.partCount = plan.partCount;
        r.overwrite = req.overwrite;
        r.extra = req.extra;
        return r;
    }

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
        if (!persistent())
            return;
        detail::serialStorePool()->start([s = store, id = req.transferId] { (void)s->remove(id); });
    }

    // --- finishing ----------------------------------------------------------------

    void stopEverything()
    {
        for (Part& p : parts) {
            if (p.request != RequestId::Invalid)
                session->cancel(p.request);
        }
        for (const RequestId id : pendingRequests)
            session->cancel(id);
        pendingRequests.clear();
    }

    void finish(Result<FileDownloadResult> r)
    {
        if (done)
            return;
        done = true;
        stopEverything();
        result = std::move(r);
        state = State::Finished;
        if (result)
            qCInfo(lcTransfer).noquote() << "downloaded" << req.bucket << req.key << result->size << "bytes";
        else
            qCInfo(lcTransfer).noquote() << "download of" << req.key << "failed:" << result.error().toString();
        QMetaObject::invokeMethod(q, &Downloader::finished, Qt::QueuedConnection);
    }

    // keepPartial: leave the .qbpart and its record for a later resume.
    void fail(Error e, bool keepPartial)
    {
        if (done)
            return;
        finish(std::move(e));   // stops the requests first
        if (!keepPartial)
            discardPartial();
    }

    void discardPartial()
    {
        storeRemove();
        if (partial.isEmpty())
            return;
        // A cancelled range may still hold the file open for a moment —
        // Windows refuses to delete it then: try again a few times.
        QThreadPool::globalInstance()->start([path = partial] {
            for (int i = 0; i < 25 && QFile::exists(path); ++i) {
                if (QFile::remove(path))
                    break;
                QThread::msleep(200);
            }
        });
    }

    // --- progress -------------------------------------------------------------------

    qint64 bytesDone() const
    {
        qint64 n = doneBytes;
        for (const Part& p : parts) {
            if (p.status == PartStatus::Fetching)
                n += p.received;
        }
        return n;
    }

    void emitProgress(bool force)
    {
        if (!force && progressTimer.isValid() && progressTimer.elapsed() < 100)
            return;
        progressTimer.start();
        Q_EMIT q->progress(bytesDone(), plan.size);
    }

    // --- the start ----------------------------------------------------------------

    void begin()
    {
        if (req.targetPath.isEmpty()) {
            fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("no target file")), false);
            return;
        }
        partial = Downloader::partialPath(req.targetPath);
        const QFileInfo target(req.targetPath);
        if (target.exists() && (target.isDir() || !req.overwrite)) {
            fail(Error::make(ErrorCategory::Conflict, QStringLiteral("%1 exists already").arg(req.targetPath)), true);
            return;
        }
        HeadObjectParams h;
        h.bucket = req.bucket;
        h.key = req.key;
        h.versionId = req.versionId;
        h.ifMatch = req.expectEtag;
        const RequestId id = session->headObject(h, q, [this](Result<ObjectMeta> head) {
            if (done)
                return;
            if (!head) {
                fail(head.error(), true);
                return;
            }
            if (head->size < 0) {
                fail(Error::make(ErrorCategory::Protocol, QStringLiteral("the server reported no object size")), true);
                return;
            }
            meta = *head;
            // Pin the version we looked at (versioned buckets): every range
            // comes from it, and If-Match guards the rest.
            if (req.versionId.isEmpty() && !meta.versionId.isEmpty() && meta.versionId != QLatin1String("null"))
                req.versionId = meta.versionId;
            md5Comparable = etagIsContentMd5(meta);
            const int objectParts = etagPartCount(meta.etag);
            if (objectParts > 1)
                probeLayout(objectParts);
            else
                planRanges();
        });
        pendingRequests.push_back(id);
    }

    // A multipart object: its part size (HEAD ?partNumber=1) lets us fetch
    // the same ranges and check the MD5-of-MD5s.
    void probeLayout(int objectParts)
    {
        HeadObjectParams h;
        h.bucket = req.bucket;
        h.key = req.key;
        h.versionId = req.versionId;
        h.partNumber = 1;
        h.ifMatch = meta.etag;
        const RequestId id = session->headObject(h, q, [this, objectParts](Result<ObjectMeta> first) {
            if (done)
                return;
            const qint64 size = meta.size;
            if (first && first->partsCount == objectParts && first->size > 0) {
                const qint64 p = first->size;
                const qint64 last = size - p * (objectParts - 1);
                if (last > 0 && last <= p) {
                    plan.size = size;
                    plan.multipart = true;
                    plan.partSize = p;
                    plan.partCount = objectParts;
                    layoutFromObject = true;
                }
            }
            if (!layoutFromObject)
                qCInfo(lcTransfer).noquote() << "no usable part layout for" << req.key << "; size check only";
            if (layoutFromObject)
                preparePartialFile(true);
            else
                planRanges();
        });
        pendingRequests.push_back(id);
    }

    void planRanges()
    {
        Result<PartPlan> p = planParts(meta.size, *session->options().profile, req.planner);
        if (!p) {
            // Beyond what ranges of the provider's maximum part size can
            // cover — still one request.
            plan = PartPlan{meta.size, false, meta.size, 1};
        } else {
            plan = *p;
        }
        preparePartialFile(true);
    }

    void preparePartialFile(bool fresh)
    {
        const QString path = partial;
        const qint64 size = plan.size;
        detail::runAsync<Result<void>>(
            QThreadPool::globalInstance(), q, [path, size, fresh] { return preparePartial(path, size, fresh); },
            [this, fresh](const Result<void>& r) {
                if (done)
                    return;
                if (!r) {
                    fail(r.error(), false);
                    return;
                }
                if (fresh) {
                    parts.assign(size_t(plan.partCount), Part());
                    doneBytes = 0;
                    doneParts = 0;
                    storeWrite([rec = record()](ResumeStore& s) { return s.begin(rec); });
                }
                emitProgress(true);
                pump();
            });
    }

    // --- resume ---------------------------------------------------------------------

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
        if (r.direction != Direction::Download || r.partCount < 1 || r.finalPath.isEmpty() || r.localPath.isEmpty()) {
            fail(Error::make(ErrorCategory::InvalidRequest, QStringLiteral("not a resumable download record")), false);
            return;
        }
        req.bucket = r.bucket;
        req.key = r.key;
        req.versionId = r.versionId;
        req.targetPath = r.finalPath;
        req.overwrite = r.overwrite;
        req.account = r.account;
        req.extra = r.extra;
        partial = r.localPath;
        plan.size = r.size;
        plan.partSize = r.partSize;
        plan.partCount = r.partCount;
        plan.multipart = r.partCount > 1;
        resumed = true;

        HeadObjectParams h;
        h.bucket = req.bucket;
        h.key = req.key;
        h.versionId = req.versionId;
        h.ifMatch = r.etag;
        const RequestId id = session->headObject(h, q, [this, recorded = state_.parts, etag = r.etag](Result<ObjectMeta> head) {
            if (done)
                return;
            if (!head) {
                if (head.error().category == ErrorCategory::PreconditionFailed) {
                    // The object changed: what we have is of another version.
                    startOver();
                    return;
                }
                fail(head.error(), true);
                return;
            }
            meta = *head;
            if (head->size != plan.size) {
                startOver();
                return;
            }
            md5Comparable = etagIsContentMd5(meta);
            const int objectParts = etagPartCount(meta.etag);
            layoutFromObject = objectParts > 1 && objectParts == plan.partCount;
            if (QFileInfo(partial).size() != plan.size) {
                startOver();
                return;
            }
            recheckRecorded(recorded);
        });
        pendingRequests.push_back(id);
    }

    void startOver()
    {
        qCInfo(lcTransfer).noquote() << "download of" << req.key << "starts over";
        restarted = true;
        resumed = false;
        req.expectEtag.clear();
        layoutFromObject = false;
        begin();
    }

    // Recorded ranges count only if the bytes on disk still hash to them
    // (the record could be ahead of a disk that lost writes).
    void recheckRecorded(const QList<PartRecord>& recorded)
    {
        parts.assign(size_t(plan.partCount), Part());
        int pending = 0;
        for (const PartRecord& r : recorded) {
            if (r.partNumber < 1 || r.partNumber > plan.partCount || r.offset != plan.partOffset(r.partNumber)
                || r.length != plan.partLength(r.partNumber))
                continue;
            Part& p = parts[size_t(r.partNumber - 1)];
            p.status = PartStatus::Checking;
            ++pending;
            const int n = r.partNumber;
            hashRange(partial, r.offset, r.length, false, q, [this, n, expected = r.md5](Result<RangeHash> h) {
                if (done)
                    return;
                Part& part = parts[size_t(n - 1)];
                if (h && h->md5 == expected) {
                    part.status = PartStatus::Done;
                    part.md5 = expected;
                    doneBytes += plan.partLength(n);
                    ++doneParts;
                } else {
                    qCInfo(lcTransfer).noquote() << "range" << n << "of" << req.key
                                                 << "on disk does not match its record; fetching it again";
                    part.status = PartStatus::Pending;
                }
                if (std::none_of(parts.begin(), parts.end(), [](const Part& x) { return x.status == PartStatus::Checking; })) {
                    qCInfo(lcTransfer).noquote() << "resuming download of" << req.key << ":" << doneParts << "of"
                                                 << plan.partCount << "ranges already there";
                    emitProgress(true);
                    pump();
                }
            });
        }
        if (pending == 0) {
            emitProgress(true);
            pump();
        }
    }

    // --- the work loop ----------------------------------------------------------

    void pump()
    {
        if (done || state == State::Idle || parts.empty())
            return;
        if (std::any_of(parts.begin(), parts.end(), [](const Part& p) { return p.status == PartStatus::Checking; }))
            return;
        if (state == State::Pausing || state == State::Paused) {
            if (state == State::Pausing && fetching == 0) {
                state = State::Paused;
                Q_EMIT q->paused();
            }
            return;
        }
        if (doneParts == plan.partCount) {
            if (!verifying)
                verify();
            return;
        }
        for (size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].status != PartStatus::Pending || parts[i].retryPending)
                continue;
            const std::optional<AdaptiveConcurrency::Slot> slot = limiter->tryAcquire();
            if (!slot)
                break;
            fetch(int(i) + 1, *slot);
        }
    }

    void fetch(int n, AdaptiveConcurrency::Slot slot)
    {
        Part& p = parts[size_t(n - 1)];
        p.status = PartStatus::Fetching;
        p.slot = slot;
        p.received = 0;
        ++fetching;
        GetObjectToFileParams g;
        g.bucket = req.bucket;
        g.key = req.key;
        g.versionId = req.versionId;
        g.filePath = partial;
        g.createFile = false;   // prepared before; never re-created after a cancel
        g.fileOffset = plan.partOffset(n);
        if (plan.multipart) {
            g.rangeStart = plan.partOffset(n);
            g.rangeLength = plan.partLength(n);
        }
        g.ifMatch = meta.etag;
        p.request = session->getObjectToFile(
            g, q,
            [this, n](Result<DownloadResult> r) {
                Part& part = parts[size_t(n - 1)];
                part.request = RequestId::Invalid;
                --fetching;
                if (part.slot && limiter) {
                    using O = AdaptiveConcurrency::Outcome;
                    const O outcome = (r && r->throttled) || (!r && isThrottle(r.error())) ? O::Throttled
                                    : r ? O::Succeeded
                                        : O::Failed;
                    limiter->release(*part.slot, outcome);
                }
                part.slot.reset();
                part.received = 0;
                if (done)
                    return;
                if (!r) {
                    rangeFailed(n, r.error());
                    return;
                }
                if (r->bytesWritten != plan.partLength(n)) {
                    rangeFailed(n, Error::make(ErrorCategory::Protocol, QStringLiteral("received %1 bytes, expected %2")
                                                                            .arg(r->bytesWritten)
                                                                            .arg(plan.partLength(n))));
                    return;
                }
                part.status = PartStatus::Done;
                part.md5 = r->md5;
                if (!plan.multipart)
                    singleIntegrity = r->integrity;
                PartRecord rec;
                rec.partNumber = n;
                rec.offset = plan.partOffset(n);
                rec.length = plan.partLength(n);
                rec.md5 = r->md5;
                storeWrite([id = req.transferId, rec](ResumeStore& s) { return s.addPart(id, rec); });
                doneBytes += plan.partLength(n);
                ++doneParts;
                emitProgress(true);
                pump();
            },
            [this, n](qint64 received, qint64) {
                parts[size_t(n - 1)].received = received;
                emitProgress(false);
            });
    }

    void rangeFailed(int n, const Error& e)
    {
        Part& p = parts[size_t(n - 1)];
        if (e.category == ErrorCategory::PreconditionFailed) {
            fail(Error::make(ErrorCategory::PreconditionFailed,
                             QStringLiteral("%1 changed on the server during the download").arg(req.key)),
                 false);
            return;
        }
        if (e.category == ErrorCategory::NotFound) {
            fail(e, false);
            return;
        }
        if (e.category == ErrorCategory::LocalIo) {
            fail(e, true);   // e.g. a full disk: keep what is there
            return;
        }
        // A corrupt single GET is a Protocol error: fetch it again.
        if ((retryable(e) || e.category == ErrorCategory::Protocol) && p.retries < kPartRetries) {
            const int delay = kRetryDelaysMs[p.retries++];
            qCInfo(lcTransfer).noquote() << "range" << n << "of" << req.key << "failed (" << e.toString() << "); again in"
                                         << delay << "ms";
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
        fail(e, retryable(e));
    }

    // --- verify and finalize ------------------------------------------------------

    void verify()
    {
        verifying = true;
        if (!plan.multipart) {
            finalize(singleIntegrity);
            return;
        }
        if (layoutFromObject) {
            QList<QByteArray> md5s;
            for (const Part& p : parts)
                md5s.append(p.md5);
            finalize(sameEtag(compositeEtag(md5s), meta.etag) ? Integrity::Md5Verified : Integrity::SizeVerified);
            return;
        }
        if (!md5Comparable) {
            finalize(Integrity::SizeVerified);
            return;
        }
        // One part on the server, ranges here: the whole file must hash to the ETag.
        hashRange(partial, 0, plan.size, false, q, [this](Result<RangeHash> h) {
            if (done)
                return;
            if (!h) {
                fail(h.error(), true);
                return;
            }
            if (!sameEtag(md5Etag(h->md5), meta.etag)) {
                storeRemove();   // the .qbpart stays for inspection
                fail(Error::make(ErrorCategory::Protocol,
                                 QStringLiteral("integrity check failed: %1 does not hash to the ETag").arg(partial)),
                     true);
                return;
            }
            finalize(Integrity::Md5Verified);
        });
    }

    void finalize(Integrity integrity)
    {
        const QString from = partial;
        const QString to = req.targetPath;
        const bool overwrite = req.overwrite;
        const qint64 size = plan.size;
        const QDateTime modified = req.keepModificationTime ? meta.lastModified : QDateTime();
        detail::runAsync<Result<void>>(
            QThreadPool::globalInstance(), q,
            [from, to, overwrite, size, modified] { return finalizeFile(from, to, overwrite, size, modified); },
            [this, integrity](const Result<void>& r) {
                if (done)
                    return;
                if (!r) {
                    fail(r.error(), true);
                    return;
                }
                storeRemove();
                FileDownloadResult out;
                out.path = req.targetPath;
                out.size = plan.size;
                out.etag = meta.etag;
                out.versionId = meta.versionId;
                out.lastModified = meta.lastModified;
                out.parts = plan.partCount;
                out.integrity = integrity;
                out.resumed = resumed;
                out.restarted = restarted;
                finish(out);
            });
    }
};

Downloader::Downloader(Session* session, DownloadRequest request, AdaptiveConcurrency* limiter,
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

Downloader::~Downloader()
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

std::unique_ptr<Downloader> Downloader::fromStore(Session* session, std::shared_ptr<ResumeStore> store,
                                                  const QString& transferId, AdaptiveConcurrency* limiter, QObject* parent)
{
    DownloadRequest r;
    r.transferId = transferId;
    auto down = std::make_unique<Downloader>(session, r, limiter, std::move(store), parent);
    down->d->fromStore = true;
    return down;
}

QString Downloader::partialPath(const QString& targetPath)
{
    return targetPath + QStringLiteral(".qbpart");
}

const DownloadRequest& Downloader::request() const
{
    return d->req;
}

Downloader::State Downloader::state() const
{
    return d->state;
}

qint64 Downloader::bytesDone() const
{
    return d->bytesDone();
}

qint64 Downloader::bytesTotal() const
{
    return d->plan.size;
}

int Downloader::partsDone() const
{
    return d->doneParts;
}

int Downloader::partCount() const
{
    return d->plan.partCount;
}

const Result<FileDownloadResult>& Downloader::result() const
{
    return d->result;
}

void Downloader::start()
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

void Downloader::pause()
{
    if (d->state != State::Running)
        return;
    d->state = State::Pausing;
    d->pump();
}

void Downloader::resume()
{
    if (d->state != State::Pausing && d->state != State::Paused)
        return;
    d->state = State::Running;
    d->pump();
}

void Downloader::cancel(bool keepForResume)
{
    if (d->done)
        return;
    d->fail(Error::make(ErrorCategory::Cancelled, QStringLiteral("download cancelled")), keepForResume);
}

} // namespace qbucket::s3::transfer
