#include "controllers/ObjectCopyRunner.h"

#include "controllers/ConnectionTester.h"
#include "controllers/Job.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"

#include <qbucket/s3/Session.h>
#include <qbucket/s3/transfer/Copier.h>

namespace qbucket {

namespace {

QString classToKeep(const QString& storageClass)
{
    // Without it a copy lands in STANDARD.
    return storageClass.isEmpty() || storageClass == QLatin1String("STANDARD") ? QString() : storageClass;
}

} // namespace

ObjectCopyRunner::ObjectCopyRunner(Job* job)
    : QObject(job),
      m_job(job)
{
}

ObjectCopyRunner::~ObjectCopyRunner() = default;

bool ObjectCopyRunner::etagIsContentHash(const QString& etag)
{
    return !etag.isEmpty() && !etag.contains(u'-');
}

void ObjectCopyRunner::run(const Item& item, Done done)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        QMetaObject::invokeMethod(this, [item, done = std::move(done)] {
            done(item, {false, tr("not copied (not connected) — the original is untouched"), false});
        }, Qt::QueuedConnection);
        return;
    }
    if (item.size > kCopyLimit) {
        copyInParts_(item, std::move(done));
        return;
    }
    s3::CopyObjectParams params;
    params.sourceBucket = item.sourceBucket;
    params.sourceKey = item.sourceKey;
    params.bucket = item.bucket;
    params.key = item.key;
    // A concurrent change of the source aborts the copy instead of copying
    // something else than what was planned.
    params.ifMatch = item.etag;
    params.storageClass = classToKeep(item.storageClass);
    session->copyObject(params, this, [this, item, done = std::move(done)](const s3::Result<s3::CopyObjectResult>& r) {
        Journal::Record rec = m_job->record_(s3::operationName(s3::Operation::CopyObject), item.key, item.bucket);
        rec.message = item.sourceBucket == item.bucket ? QStringLiteral("from %1").arg(item.sourceKey)
                                                       : QStringLiteral("from s3://%1/%2").arg(item.sourceBucket, item.sourceKey);
        if (!r) {
            const s3::Error& err = r.error();
            rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.requestId = err.requestId;
            rec.message += QStringLiteral(": ") + err.toString();
            m_job->journal_({rec});
            done(item, {false,
                        err.category == s3::ErrorCategory::PreconditionFailed
                            ? tr("changed since it was planned — left alone")
                            : tr("not copied (%1) — the original is untouched").arg(ConnectionTester::describe(err)),
                        false});
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        rec.versionId = r->versionId;
        m_job->journal_({rec});
        copied_(item, false, done);
    });
}

// Above CopyObject's limit: UploadPartCopy in ranges, every part with the
// source's If-Match.
void ObjectCopyRunner::copyInParts_(const Item& item, Done done)
{
    s3::Session* session = m_job->session_();
    TransferService* transfers = m_job->controller_() ? m_job->controller_()->transfers() : nullptr;
    if (!session || !transfers) {
        QMetaObject::invokeMethod(this, [item, done = std::move(done)] {
            done(item, {false, tr("not copied (not connected) — the original is untouched"), false});
        }, Qt::QueuedConnection);
        return;
    }
    s3::transfer::CopyRequest req;
    req.sourceBucket = item.sourceBucket;
    req.sourceKey = item.sourceKey;
    req.bucket = item.bucket;
    req.key = item.key;
    req.ifMatch = item.etag;
    req.storageClass = classToKeep(item.storageClass);
    auto copier = std::make_unique<s3::transfer::Copier>(session, req, transfers->limiter(m_job->account().id));
    s3::transfer::Copier* raw = copier.get();
    connect(raw, &s3::transfer::Copier::finished, this, [this, raw, item, done = std::move(done)] {
        const s3::Result<s3::transfer::CopyResult>& r = raw->result();
        Journal::Record rec = m_job->record_(QStringLiteral("MultipartCopy"), item.key, item.bucket);
        rec.message = item.sourceBucket == item.bucket ? QStringLiteral("from %1").arg(item.sourceKey)
                                                       : QStringLiteral("from s3://%1/%2").arg(item.sourceBucket, item.sourceKey);
        if (r) {
            rec.httpStatus = 200;
            rec.versionId = r->versionId;
            rec.message += QStringLiteral(", %1 parts").arg(r->parts);
            m_job->journal_({rec});
            copied_(item, true, done);
        } else {
            const s3::Error& err = r.error();
            rec.result = Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.message += QStringLiteral(": ") + err.toString();
            m_job->journal_({rec});
            const bool cancelled = err.category == s3::ErrorCategory::Cancelled;
            done(item, {false,
                        cancelled ? QString()
                        : err.category == s3::ErrorCategory::PreconditionFailed
                            ? tr("changed since it was planned — left alone")
                            : tr("not copied (%1) — the original is untouched").arg(ConnectionTester::describe(err)),
                        cancelled});
        }
        // Not from inside its own signal.
        for (auto it = m_copiers.begin(); it != m_copiers.end(); ++it) {
            if (it->get() == raw) {
                it->release()->deleteLater();
                m_copiers.erase(it);
                break;
            }
        }
    });
    m_copiers.push_back(std::move(copier));
    raw->start();
}

void ObjectCopyRunner::copied_(const Item& item, bool inParts, Done done)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        done(item, {false, item.deleteSource ? tr("copied, but not verified (not connected) — the original is kept")
                                              : tr("copied, but not verified (not connected)"),
                    false});
        return;
    }
    // Verify the copy before anything else happens.
    session->headObject(item.bucket, item.key, QString(), this,
                        [this, item, inParts, done = std::move(done)](const s3::Result<s3::ObjectMeta>& head) {
        const bool sizeOk = head && head->size == item.size;
        // A copy in parts has an ETag of its own; the Copier guarded each
        // part with the source's ETag and checked the size.
        const bool etagOk = inParts || !etagIsContentHash(item.etag) || (head && head->etag == item.etag);
        if (!sizeOk || !etagOk) {
            const QString kept = item.deleteSource ? tr(" — the original is kept") : QString();
            done(item, {false,
                        head ? tr("the copy does not match (size or checksum), check “%1”").arg(item.key) + kept
                             : tr("the copy could not be read back") + kept,
                        false});
            return;
        }
        if (!item.deleteSource) {
            done(item, {true, {}, false});
            return;
        }
        deleteSource_(item, done);
    });
}

void ObjectCopyRunner::deleteSource_(const Item& item, Done done)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        done(item, {false, tr("copied and verified, but the original could not be deleted (not connected)"), false});
        return;
    }
    session->deleteObject(item.sourceBucket, item.sourceKey, QString(), this,
                          [this, item, done = std::move(done)](const s3::Result<s3::DeleteObjectResult>& r) {
        Journal::Record rec = m_job->record_(s3::operationName(s3::Operation::DeleteObject), item.sourceKey,
                                             item.sourceBucket);
        Result out;
        if (r) {
            out.ok = true;
            rec.httpStatus = 204;
            rec.requestId = r->requestId;
            rec.versionId = r->versionId;
        } else {
            const s3::Error& err = r.error();
            rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.requestId = err.requestId;
            rec.message = err.toString();
            out.failure = tr("copied, but the original could not be deleted: %1").arg(ConnectionTester::describe(err));
        }
        m_job->journal_({rec});
        done(item, out);
    });
}

void ObjectCopyRunner::cancel()
{
    for (const auto& c : m_copiers)
        c->cancel();   // aborts its upload; the source stays
}

} // namespace qbucket
