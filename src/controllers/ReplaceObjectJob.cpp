#include "controllers/ReplaceObjectJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/InPlaceCopy.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"

#include <qbucket/s3/transfer/Uploader.h>

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
#include <QTimer>

namespace qbucket {

namespace {

QString lastSegment(const QString& key)
{
    return key.section(u'/', -1);
}

} // namespace

ReplaceObjectJob::ReplaceObjectJob(const Account& account, const QString& bucket, const Request& request,
                                   QObject* parent)
    : Job(account, bucket, parent),
      m_request(request)
{
}

ReplaceObjectJob::~ReplaceObjectJob() = default;

QString ReplaceObjectJob::title() const
{
    return tr("Upload the changes to “%1”").arg(lastSegment(m_request.key));
}

QList<s3::Feature> ReplaceObjectJob::requiredFeatures() const
{
    QList<s3::Feature> out;
    if (m_snapshot && !m_snapshot->tags.value_or(s3::TagSet()).isEmpty())
        out.append(s3::Feature::ObjectTaggingWrite);
    if (!m_canned.isEmpty())
        out.append(s3::Feature::ObjectAclWrite);
    return out;
}

bool ReplaceObjectJob::needsConfirmation() const
{
    // An edit saved on a versioned bucket of a non-production account loses
    // nothing; everything else is shown first.
    return m_conflict != Conflict::None || !m_versioned || account().environment == Environment::Production;
}

// --- plan -----------------------------------------------------------------------------

void ReplaceObjectJob::plan()
{
    m_run = std::make_unique<QObject>();
    const QFileInfo local(m_request.localPath);
    if (!local.isFile()) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("The local file is gone: %1").arg(QDir::toNativeSeparators(m_request.localPath))); },
                                  Qt::QueuedConnection);
        return;
    }
    m_localSize = local.size();
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 600, m_run.get(), [this](bool) {
        if (phase() != Phase::Planning)
            return;
        CapabilityController* caps = controller_()->capabilities();
        const auto state = [&](s3::Feature f) { return caps->entry(account().id, bucket(), f).state; };
        if (state(s3::Feature::ObjectTagging) == s3::CapabilityState::NotPermitted
            || state(s3::Feature::ObjectAcl) == s3::CapabilityState::NotPermitted) {
            fail_(tr("These credentials may not read the object's tags or permissions: replacing it could lose "
                     "them. Nothing was uploaded."));
            return;
        }
        const bool readTags = state(s3::Feature::ObjectTagging) != s3::CapabilityState::Unsupported;
        const bool readAcl = state(s3::Feature::ObjectAcl) != s3::CapabilityState::Unsupported;
        m_writeTags = caps->decide(account().id, bucket(), s3::Feature::ObjectTaggingWrite, true) == s3::Decision::Allowed;
        m_writeAcl = caps->decide(account().id, bucket(), s3::Feature::ObjectAclWrite, true) == s3::Decision::Allowed;
        const CapabilityController::Versioning v = caps->versioningOf(account().id, bucket());
        m_versioned = v.known && v.status == s3::VersioningStatus::Enabled;
        s3::Session* session = session_();
        if (!session) {
            fail_(tr("Not connected."));
            return;
        }
        ObjectSnapshot::read(session, bucket(), m_request.key, readTags, readAcl, m_run.get(),
                             [this](const s3::Result<ObjectSnapshot>& r) { planned_(r); });
    });
}

void ReplaceObjectJob::planned_(const s3::Result<ObjectSnapshot>& snapshot)
{
    if (phase() != Phase::Planning)
        return;
    if (!snapshot && snapshot.error().category != s3::ErrorCategory::NotFound) {
        fail_(tr("“%1” could not be read: %2").arg(m_request.key, ConnectionTester::describe(snapshot.error())));
        return;
    }
    if (!snapshot) {
        m_conflict = Conflict::Deleted;
    } else {
        m_snapshot = *snapshot;
        if (m_snapshot->meta.etag != m_request.openedEtag)
            m_conflict = Conflict::Changed;
        const QString why = InPlaceCopy::cannotCarry(*m_snapshot, account().providerId, m_writeTags, m_writeAcl, true);
        if (!why.isEmpty()) {
            fail_(tr("“%1” can't be replaced in place (%2). Upload the file under another name instead.")
                      .arg(m_request.key, why));
            return;
        }
        if (m_snapshot->acl) {
            const std::optional<QString> canned = s3::cannedAclOf(*m_snapshot->acl);
            if (canned && *canned != QLatin1String("private"))
                m_canned = *canned;
        }
    }

    Preview p;
    p.action = tr("Upload Changes");
    p.scope = QStringLiteral("%1 → s3://%2/%3").arg(QDir::toNativeSeparators(m_request.localPath), bucket(), m_request.key);
    p.objects = 1;
    p.bytes = m_localSize;
    p.samples = {m_request.key};
    if (m_snapshot) {
        const s3::ObjectMeta& m = m_snapshot->meta;
        QStringList kept;
        if (!m.contentType.isEmpty())
            kept.append(m.contentType);
        if (!m.userMetadata.isEmpty())
            kept.append(tr("%n metadata entry(s)", nullptr, int(m.userMetadata.size())));
        if (!m.storageClass.isEmpty())
            kept.append(m.storageClass);
        if (const qsizetype tags = m_snapshot->tags.value_or(s3::TagSet()).size(); tags > 0)
            kept.append(tr("%n tag(s)", nullptr, int(tags)));
        if (!m_canned.isEmpty())
            kept.append(m_canned);
        if (!kept.isEmpty())
            p.details.append(tr("Kept: %1").arg(kept.join(QStringLiteral(", "))));
    }
    if (m_conflict == Conflict::Changed && m_snapshot) {
        p.conflicts = 1;
        p.conflictSamples = {tr("%1 — changed on the server since you opened it (%2)")
                                 .arg(m_request.key, QLocale().toString(m_snapshot->meta.lastModified.toLocalTime(),
                                                                        QLocale::ShortFormat))};
    } else if (m_conflict == Conflict::Deleted) {
        p.conflicts = 1;
        p.conflictSamples = {tr("%1 — deleted on the server since you opened it").arg(m_request.key)};
    }
    if (m_versioned)
        p.details.append(tr("The bucket keeps versions: the current content stays as an earlier version."));
    else if (m_conflict != Conflict::Deleted)
        p.warnings.append(tr("The current content is replaced for good: the bucket keeps no versions."));
    setProgress_(0, 1);
    setBytes_(0, m_localSize);
    setPreview_(p);
}

// --- execute --------------------------------------------------------------------------

void ReplaceObjectJob::execute()
{
    if (m_conflict != Conflict::None && conflictPolicy() == ConflictPolicy::Skip) {
        Report r;
        r.skipped = 1;
        r.notes.append(tr("%1: changed on the server since it was opened — not uploaded").arg(m_request.key));
        finish_(r);
        return;
    }
    m_targetKey = m_request.key;
    if (m_conflict != Conflict::None && conflictPolicy() == ConflictPolicy::KeepBoth) {
        findFreeKey_(1);
        return;
    }
    guardAndUpload_();
}

// Overwrite only what the plan saw: If-Match where the server honours it,
// otherwise a HEAD right before.
void ReplaceObjectJob::guardAndUpload_()
{
    CapabilityController* caps = controller_()->capabilities();
    if (m_conflict == Conflict::Deleted) {
        // Recreate: the key must still be free.
        if (caps->decide(account().id, bucket(), s3::Feature::ConditionalWrites, false) == s3::Decision::Allowed) {
            upload_(false, true);
            return;
        }
    } else if (caps->decide(account().id, bucket(), s3::Feature::ConditionalOverwrite, false) == s3::Decision::Allowed) {
        upload_(true, false);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        fail_report_(tr("Not connected."));
        return;
    }
    session->headObject(bucket(), m_targetKey, QString(), m_run.get(), [this](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Executing)
            return;
        if (isCancelled()) {
            finish_({});
            return;
        }
        const bool absent = !head && head.error().category == s3::ErrorCategory::NotFound;
        if (!head && !absent) {
            fail_report_(ConnectionTester::describe(head.error()));
            return;
        }
        const bool asPlanned = m_conflict == Conflict::Deleted ? absent : (head && head->etag == m_snapshot->meta.etag);
        if (!asPlanned) {
            fail_report_(tr("changed on the server again meanwhile — not uploaded"));
            return;
        }
        upload_(false, false);
    });
}

void ReplaceObjectJob::findFreeKey_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        fail_report_(tr("Not connected."));
        return;
    }
    if (attempt >= 100) {
        fail_report_(tr("no free name for a copy"));
        return;
    }
    const QString key = UploadJob::keepBothName(m_request.key, attempt);
    session->headObject(bucket(), key, QString(), m_run.get(), [this, key, attempt](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Executing)
            return;
        if (isCancelled()) {
            finish_({});
            return;
        }
        if (head) {
            findFreeKey_(attempt + 1);
            return;
        }
        if (head.error().category != s3::ErrorCategory::NotFound) {
            fail_report_(ConnectionTester::describe(head.error()));
            return;
        }
        m_targetKey = key;
        const bool conditional = controller_()->capabilities()->decide(account().id, bucket(), s3::Feature::ConditionalWrites,
                                                                       false) == s3::Decision::Allowed;
        upload_(false, conditional);
    });
}

void ReplaceObjectJob::upload_(bool ifMatch, bool ifNoneMatch)
{
    s3::Session* session = session_();
    TransferService* transfers = controller_()->transfers();
    if (!session || !transfers) {
        fail_report_(tr("Not connected."));
        return;
    }
    s3::transfer::UploadRequest req;
    req.bucket = bucket();
    req.key = m_targetKey;
    req.localPath = m_request.localPath;
    req.contentType.clear();
    s3::ObjectHeaders headers;
    if (m_snapshot) {
        headers = s3::objectHeadersOf(m_snapshot->meta);
        req.storageClass = InPlaceCopy::classToSend(m_snapshot->meta.storageClass);
        if (m_snapshot->meta.serverSideEncryption == QLatin1String("AES256"))
            req.serverSideEncryption = m_snapshot->meta.serverSideEncryption;
    }
    if (headers.contentType.isEmpty())
        headers.contentType = QMimeDatabase().mimeTypeForFile(m_request.localPath, QMimeDatabase::MatchExtension).name();
    req.headers = headers;
    req.cannedAcl = m_canned;
    req.ifMatch = ifMatch && m_snapshot ? m_snapshot->meta.etag : QString();
    req.ifNoneMatch = ifNoneMatch;
    req.account = account().id.toString(QUuid::WithoutBraces);
    // Not resumable: a changed file is uploaded again from the start.
    m_uploader = std::make_unique<s3::transfer::Uploader>(session, req, transfers->limiter(account().id), nullptr);
    connect(m_uploader.get(), &s3::transfer::Uploader::finished, this, &ReplaceObjectJob::uploaded_);
    connect(m_uploader.get(), &s3::transfer::Uploader::progress, this,
            [this](qint64 done, qint64 total) { setBytes_(done, total); });
    m_uploader->start();
}

void ReplaceObjectJob::uploaded_()
{
    if (phase() != Phase::Executing || !m_uploader)
        return;
    const s3::Result<s3::transfer::UploadResult> r = m_uploader->result();
    m_uploader.release()->deleteLater();
    Journal::Record rec = record_(s3::operationName(s3::Operation::PutObject), m_targetKey);
    if (!r) {
        const s3::Error& e = r.error();
        rec.result = e.category == s3::ErrorCategory::Cancelled ? Journal::Result::Skipped : Journal::Result::Failed;
        rec.httpStatus = e.httpStatus;
        rec.requestId = e.requestId;
        rec.message = QStringLiteral("replace from %1: %2").arg(m_request.localPath, e.toString());
        journal_({rec});
        if (e.category == s3::ErrorCategory::Cancelled) {
            finish_({});
            return;
        }
        fail_report_(e.category == s3::ErrorCategory::PreconditionFailed
                         ? tr("changed on the server meanwhile — not uploaded")
                         : ConnectionTester::describe(e));
        return;
    }
    rec.httpStatus = 200;
    rec.requestId = r->requestId;
    rec.versionId = r->versionId;
    rec.message = QStringLiteral("replace from %1, %2, %3")
                      .arg(m_request.localPath, r->etag,
                           r->integrity == s3::Integrity::Md5Verified ? QStringLiteral("md5 verified")
                                                                      : QStringLiteral("size verified"));
    journal_({rec});
    m_newEtag = r->etag;
    setProgress_(1, 1);
    if (m_snapshot && !m_snapshot->tags.value_or(s3::TagSet()).isEmpty()) {
        writeTags_();
        return;
    }
    executed_();
}

// A PUT carries no tags here (RGW and Hetzner leave x-amz-tagging unreadable):
// written back on their own.
void ReplaceObjectJob::writeTags_()
{
    s3::Session* session = session_();
    if (!session) {
        executed_();   // verify reports the missing tags
        return;
    }
    s3::PutObjectTaggingParams p;
    p.bucket = bucket();
    p.key = m_targetKey;
    p.tags = m_snapshot ? m_snapshot->tags.value_or(s3::TagSet()) : s3::TagSet();
    session->putObjectTagging(p, m_run.get(), [this](const s3::Result<s3::SubresourceWriteResult>& r) {
        if (phase() != Phase::Executing)
            return;
        Journal::Record rec = record_(s3::operationName(s3::Operation::PutObjectTagging), m_targetKey);
        rec.message = QStringLiteral("tags carried over (replace)");
        if (r) {
            rec.httpStatus = 200;
            rec.requestId = r->requestId;
        } else {
            rec.result = Journal::Result::Failed;
            rec.httpStatus = r.error().httpStatus;
            rec.message += QStringLiteral(": ") + r.error().toString();
        }
        journal_({rec});
        executed_();
    });
}

// --- verify ---------------------------------------------------------------------------

void ReplaceObjectJob::verify()
{
    check_();
}

void ReplaceObjectJob::check_()
{
    s3::Session* session = session_();
    if (!session) {
        Report r;
        r.succeeded = 1;
        r.verification = tr("Uploaded, but not checked (not connected).");
        finish_(r);
        return;
    }
    const bool tags = m_snapshot && !m_snapshot->tags.value_or(s3::TagSet()).isEmpty();
    const bool acl = !m_canned.isEmpty();
    ObjectSnapshot::read(session, bucket(), m_targetKey, tags, acl, m_run.get(), [this, tags, acl](s3::Result<ObjectSnapshot> now) {
        if (phase() != Phase::Verifying)
            return;
        QStringList wrong;
        if (!now) {
            wrong.append(ConnectionTester::describe(now.error()));
        } else {
            const s3::ObjectMeta& m = now->meta;
            if (m.size != m_localSize)
                wrong.append(tr("size"));
            if (m.etag != m_newEtag)
                wrong.append(tr("content (ETag)"));
            if (m_snapshot) {
                wrong += InPlaceCopy::headerDifferences(s3::objectHeadersOf(m_snapshot->meta), s3::objectHeadersOf(m));
                if (!InPlaceCopy::sameClass(m.storageClass, m_snapshot->meta.storageClass))
                    wrong.append(tr("storage class (%1)").arg(m.storageClass.isEmpty() ? QStringLiteral("STANDARD") : m.storageClass));
            }
            if (tags && (!now->tags || !InPlaceCopy::sameTags(*now->tags, *m_snapshot->tags)))
                wrong.append(tr("tags"));
            if (acl && (!now->acl || s3::cannedAclOf(*now->acl) != std::optional<QString>(m_canned)))
                wrong.append(tr("permissions"));
        }
        if (!wrong.isEmpty() && !m_rechecked) {
            // Reads may lag behind a write for a moment.
            m_rechecked = true;
            QTimer::singleShot(m_recheckMs, m_run.get(), [this] { check_(); });
            return;
        }
        Report r;
        r.succeeded = 1;
        r.verified = wrong.isEmpty();
        if (m_targetKey != m_request.key)
            r.notes.append(tr("Uploaded as “%1”; “%2” is unchanged.").arg(m_targetKey, m_request.key));
        r.verification = wrong.isEmpty()
            ? tr("Read back: content, size, headers, class, tags and permissions as intended.")
            : tr("Uploaded, but it doesn't match: %1").arg(wrong.join(QStringLiteral(", ")));
        finish_(r);
    });
}

void ReplaceObjectJob::onCancel()
{
    // Before execution Job::cancel() finishes at once; an upload under way
    // reports through uploaded_(), a HEAD in flight through its answer.
    if (m_uploader)
        m_uploader->cancel();
    if (phase() != Phase::Executing && phase() != Phase::Verifying)
        m_run.reset();
}

void ReplaceObjectJob::fail_report_(const QString& why)
{
    Report r;
    r.failed = 1;
    r.failures.append(QStringLiteral("%1: %2").arg(m_targetKey.isEmpty() ? m_request.key : m_targetKey, why));
    finish_(r);
}

} // namespace qbucket
