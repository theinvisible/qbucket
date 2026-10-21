#include "controllers/DeleteBucketJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/VersionPurger.h"
#include "controllers/VersionWalk.h"

#include <QLocale>
#include <QTimer>

namespace qbucket {

DeleteBucketJob::DeleteBucketJob(const Account& account, const QString& bucket, bool emptyFirst,
                                 const QString& planDir, QObject* parent)
    : Job(account, bucket, parent),
      m_emptyFirst(emptyFirst),
      m_planDir(planDir)
{
}

DeleteBucketJob::~DeleteBucketJob() = default;

QString DeleteBucketJob::title() const
{
    return m_emptyFirst ? tr("Delete bucket “%1” with everything in it").arg(bucket())
                        : tr("Delete bucket “%1”").arg(bucket());
}

// --- plan ------------------------------------------------------------------------

void DeleteBucketJob::plan()
{
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 60, m_run.get(), [this](bool fresh) {
        if (!fresh) {
            fail_(tr("The bucket's state could not be checked. Nothing was changed."));
            return;
        }
        m_currentOnly = controller_()->capabilities()->entry(account().id, bucket(), s3::Feature::ObjectVersionsListing).state
                        == s3::CapabilityState::Unsupported;
        walk_();
    });
}

void DeleteBucketJob::walk_()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    m_walk = std::make_unique<VersionWalk>(*session, bucket(), QString(), false, m_run.get(),
                                           m_currentOnly ? VersionWalk::Source::CurrentOnly : VersionWalk::Source::Versions);
    m_walk->run(
        [this](const QList<s3::VersionEntry>& entries, bool last) {
            if (phase() != Phase::Planning)
                return false;
            QList<QStringList> records;
            records.reserve(entries.size());
            for (const s3::VersionEntry& e : entries) {
                records.append(VersionPurger::record({e.key, e.versionId, e.isDeleteMarker, e.isLatest, e.size, {}}));
                if (e.isDeleteMarker) {
                    ++m_markers;
                } else {
                    ++m_versions;
                    m_bytes += e.size;
                }
                if (m_samples.size() < kSamples)
                    m_samples.append(e.versionId.isEmpty() || e.versionId == QLatin1String("null")
                                         ? e.key
                                         : tr("%1 (version %2)").arg(e.key, e.versionId));
            }
            m_plan->appendRecords(records);
            setProgress_(m_plan->count(), 0);
            if (!last)
                return true;
            QMetaObject::invokeMethod(this, [this] {
                m_walk.reset();
                listUploads_({}, {});
            }, Qt::QueuedConnection);
            return false;
        },
        [this](const s3::Error& e) {
            if (phase() == Phase::Planning)
                fail_(tr("Could not list the bucket: %1").arg(ConnectionTester::describe(e)));
        });
}

void DeleteBucketJob::listUploads_(const QString& keyMarker, const QString& uploadIdMarker)
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    if (controller_()->capabilities()->entry(account().id, bucket(), s3::Feature::MultipartListing).state
        == s3::CapabilityState::Unsupported) {
        planDone_();
        return;
    }
    s3::ListMultipartUploadsParams p;
    p.bucket = bucket();
    p.keyMarker = keyMarker;
    p.uploadIdMarker = uploadIdMarker;
    session->listMultipartUploads(p, m_run.get(), [this](const s3::Result<s3::ListMultipartUploadsPage>& r) {
        if (phase() != Phase::Planning)
            return;
        if (!r) {
            fail_(tr("Could not list the unfinished uploads: %1").arg(ConnectionTester::describe(r.error())));
            return;
        }
        for (const s3::MultipartUploadInfo& u : r->uploads)
            m_uploads.append({u.key, u.uploadId});
        if (m_uploads.size() > kMaxUploads) {
            fail_(tr("More than %1 unfinished uploads: abort them first (Tools → Incomplete Uploads).")
                      .arg(QLocale().toString(kMaxUploads)));
            return;
        }
        if (r->isTruncated) {
            listUploads_(r->nextKeyMarker, r->nextUploadIdMarker);
            return;
        }
        planDone_();
    });
}

void DeleteBucketJob::planDone_()
{
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        const bool empty = m_plan->count() == 0 && m_uploads.isEmpty();
        const QLocale locale;
        if (!empty && !m_emptyFirst) {
            fail_((m_currentOnly ? tr("The bucket isn't empty (objects: %1, unfinished uploads: %2). Delete what is in "
                                      "it as well, or empty it first.")
                                 : tr("The bucket isn't empty (versions and delete markers: %1, unfinished uploads: "
                                      "%2). Delete what is in it as well, or empty it first."))
                      .arg(locale.toString(m_plan->count()), locale.toString(m_uploads.size())));
            return;
        }
        const CapabilityController::Versioning v = controller_()->capabilities()->versioningOf(account().id, bucket());
        if (m_plan->count() > 0 && v.mfaDelete.value_or(false)) {
            fail_(tr("MFA delete is enabled on this bucket: deleting its versions needs a code from the MFA device, "
                     "which QBucket can't send. Nothing was changed."));
            return;
        }
        Preview p;
        p.action = tr("Delete Bucket");
        p.scope = QStringLiteral("s3://%1").arg(bucket());
        p.permanent = true;
        p.typeToConfirm = bucket();   // always typed
        if (empty) {
            p.objects = -1;
            p.details.append(tr("The bucket is empty."));
            p.permanentText = tr("The bucket is deleted for good; its name may be taken by someone else afterwards.");
        } else {
            p.objects = m_plan->count();
            p.bytes = m_bytes;
            p.samples = m_samples;
            p.amount = m_currentOnly ? tr("%n object(s)", nullptr, int(m_versions))
                                     : tr("%n version(s)", nullptr, int(m_versions));
            if (m_markers > 0)
                p.amount = tr("%1, %n delete marker(s)", nullptr, int(m_markers)).arg(p.amount);
            if (!m_uploads.isEmpty())
                p.amount = tr("%1, %n unfinished upload(s)", nullptr, int(m_uploads.size())).arg(p.amount);
            if (m_plan->count() == 0)
                p.objects = -1;   // uploads only: the amount says it
            p.permanentText = tr("Everything in the bucket is deleted for good — every version, also on a versioned "
                                 "bucket — and then the bucket itself.");
            if (v.objectLock.value_or(false)) {
                p.warnings.append(tr("Object lock is enabled: versions under retention or a legal hold can't be "
                                     "deleted, and the bucket stays — unless GOVERNANCE retention is bypassed below."));
                p.bypassOffer = true;
            }
        }
        p.details.append(tr("Before the bucket itself is deleted, QBucket lists it again to be sure it is empty."));
        setPreview_(p);
    });
}

// --- execute ---------------------------------------------------------------------------

void DeleteBucketJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    if (m_settleMs < 0)
        m_settleMs = session->options().profile->bucketEmptinessSettleSecs() * 1000;
    if (m_plan->count() == 0) {
        abortUploads_();
        return;
    }
    VersionPurger::Options o;
    o.useBatches = controller_()->capabilities()->decide(account().id, bucket(), s3::Feature::BatchDelete, true)
                   == s3::Decision::Allowed;
    o.batchSize = session->options().profile->maxDeleteBatch();
    o.bypassGovernance = bypassGovernance();
    o.markerMessage = QStringLiteral("delete marker removed (emptying the bucket)");
    m_purger = new VersionPurger(this, m_plan.get(), o);
    connect(m_purger, &VersionPurger::progressed, this, [this](qint64 n) { setProgress_(n, progressTotal()); });
    setProgress_(0, m_plan->count() + m_uploads.size());
    m_purger->run([this] {
        if (phase() != Phase::Executing)
            return;
        m_failed = m_purger->failedSet().size();
        m_failures = m_purger->failures();
        if (isCancelled()) {
            m_verification = tr("cancelled — the bucket stays; what was deleted is in the journal");
            finish_(report_());
            return;
        }
        abortUploads_();
    });
}

void DeleteBucketJob::abortUploads_()
{
    m_abortIndex = 0;
    abortNext_();
}

void DeleteBucketJob::abortNext_()
{
    if (isCancelled()) {
        m_verification = tr("cancelled — the bucket stays; what was deleted is in the journal");
        finish_(report_());
        return;
    }
    if (m_abortIndex >= m_uploads.size()) {
        if (m_failed > 0) {
            m_verification = tr("the bucket stays: %n version(s) could not be deleted", nullptr, int(m_failed));
            finish_(report_());
            return;
        }
        beforeDelete_(1);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    const Upload u = m_uploads[m_abortIndex++];
    session->abortMultipartUpload(bucket(), u.key, u.uploadId, m_run.get(), [this, u](const s3::Result<void>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::AbortMultipartUpload), u.key);
        rec.message = QStringLiteral("upload %1 (emptying the bucket)").arg(u.uploadId);
        // Gone already is as good as aborted.
        if (r || r.error().category == s3::ErrorCategory::NotFound) {
            ++m_aborted;
            rec.httpStatus = r ? 204 : 404;
        } else {
            ++m_failed;
            rec.result = Journal::Result::Failed;
            rec.httpStatus = r.error().httpStatus;
            rec.message += QStringLiteral(": ") + r.error().toString();
            if (m_failures.size() < kMaxListed)
                m_failures.append(tr("%1 (unfinished upload): %2").arg(u.key, ConnectionTester::describe(r.error())));
        }
        journal_({rec});
        setProgress_(progressDone() + 1, progressTotal());
        abortNext_();
    });
}

void DeleteBucketJob::checkEmpty_(const std::function<void(bool empty, const QString& left)>& done)
{
    s3::Session* session = session_();
    if (!session) {
        done(false, tr("not connected"));
        return;
    }
    auto uploads = [this, done] {
        s3::Session* s = session_();
        if (!s) {
            done(false, tr("not connected"));
            return;
        }
        if (controller_()->capabilities()->entry(account().id, bucket(), s3::Feature::MultipartListing).state
            == s3::CapabilityState::Unsupported) {
            done(true, {});
            return;
        }
        s3::ListMultipartUploadsParams p;
        p.bucket = bucket();
        p.maxUploads = 1;
        s->listMultipartUploads(p, m_run.get(), [done](const s3::Result<s3::ListMultipartUploadsPage>& r) {
            if (!r)
                done(false, ConnectionTester::describe(r.error()));
            else if (!r->uploads.isEmpty())
                done(false, DeleteBucketJob::tr("an unfinished upload of “%1”").arg(r->uploads.first().key));
            else
                done(true, {});
        });
    };
    if (m_currentOnly) {
        s3::ListObjectsParams p;
        p.bucket = bucket();
        p.maxKeys = 1;
        session->listObjects(p, m_run.get(), [done, uploads](const s3::Result<s3::ListPage>& r) {
            if (!r)
                done(false, ConnectionTester::describe(r.error()));
            else if (!r->objects.isEmpty())
                done(false, DeleteBucketJob::tr("“%1”").arg(r->objects.first().key));
            else
                uploads();
        });
        return;
    }
    s3::ListObjectVersionsParams p;
    p.bucket = bucket();
    p.maxKeys = 1;
    session->listObjectVersions(p, m_run.get(), [done, uploads](const s3::Result<s3::ListObjectVersionsPage>& r) {
        if (!r)
            done(false, ConnectionTester::describe(r.error()));
        else if (!r->entries.isEmpty())
            done(false, DeleteBucketJob::tr("“%1”").arg(r->entries.first().key));
        else
            uploads();
    });
}

// Empty now, and still empty after the server's emptiness check has caught
// up (Hetzner) — only then DeleteBucket.
void DeleteBucketJob::beforeDelete_(int attempt)
{
    checkEmpty_([this, attempt](bool empty, const QString& left) {
        if (isCancelled()) {
            m_verification = tr("cancelled — the bucket stays");
            finish_(report_());
            return;
        }
        if (!empty) {
            if (attempt <= kRetries) {   // listings may lag behind the deletes
                QTimer::singleShot(m_retryMs * attempt, m_run.get(), [this, attempt] { beforeDelete_(attempt + 1); });
                return;
            }
            fail_(tr("The bucket isn't empty yet (%1) — it stays. Something may have been written meanwhile.").arg(left));
            return;
        }
        QTimer::singleShot(m_settleMs, m_run.get(), [this, attempt] {
            checkEmpty_([this, attempt](bool stillEmpty, const QString& now) {
                if (isCancelled()) {
                    m_verification = tr("cancelled — the bucket stays");
                    finish_(report_());
                    return;
                }
                if (!stillEmpty) {
                    fail_(tr("Something appeared in the bucket meanwhile (%1) — it stays.").arg(now));
                    return;
                }
                deleteBucket_(attempt);
            });
        });
    });
}

void DeleteBucketJob::deleteBucket_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    session->deleteBucket(bucket(), m_run.get(), [this, attempt](const s3::Result<void>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::DeleteBucket));
        if (r) {
            rec.httpStatus = 204;
            journal_({rec});
            m_bucketDeleted = true;
            executed_();
            return;
        }
        const s3::Error& e = r.error();
        rec.result = Journal::Result::Failed;
        rec.httpStatus = e.httpStatus;
        rec.requestId = e.requestId;
        rec.message = e.toString();
        journal_({rec});
        if (e.code == QLatin1String("BucketNotEmpty") && attempt <= kRetries) {
            QTimer::singleShot(m_retryMs * attempt, m_run.get(), [this, attempt] { beforeDelete_(attempt + 1); });
            return;
        }
        if (e.category == s3::ErrorCategory::NotFound) {
            m_bucketDeleted = true;   // gone already (a retry after a lost answer)
            executed_();
            return;
        }
        fail_(tr("The bucket could not be deleted: %1").arg(ConnectionTester::describe(e)));
    });
}

void DeleteBucketJob::onCancel()
{
    if (phase() == Phase::Planning) {
        m_walk.reset();
        m_run.reset();
    } else if (phase() == Phase::Executing && m_purger) {
        m_purger->cancel();
    }
}

// --- verify ---------------------------------------------------------------------------------

void DeleteBucketJob::verify()
{
    verifyGone_(1);
}

void DeleteBucketJob::verifyGone_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not checked (not connected)");
        finish_(report_());
        return;
    }
    session->headBucket(bucket(), m_run.get(), [this, attempt](const s3::Result<void>& r) {
        const bool gone = !r && r.error().category == s3::ErrorCategory::NotFound;
        if (!gone && attempt <= kRetries) {
            QTimer::singleShot(m_retryMs, m_run.get(), [this, attempt] { verifyGone_(attempt + 1); });
            return;
        }
        controller_()->capabilities()->forgetBucket(account().id, bucket());
        m_verified = gone;
        m_verification = gone ? tr("the bucket is gone") : tr("the bucket still answers — check again in a few minutes");
        finish_(report_());
    });
}

Job::Report DeleteBucketJob::report_() const
{
    Report r;
    r.succeeded = (m_purger ? m_purger->succeeded() : 0) + m_aborted + (m_bucketDeleted ? 1 : 0);
    r.failed = m_failed;
    r.failures = m_failures;
    r.verified = m_verified;
    r.verification = m_verification;
    return r;
}

} // namespace qbucket
