#include "controllers/DeleteJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"

#include <QTimer>

#include <algorithm>

namespace qbucket {

namespace {

QString lastSegment(QString key)
{
    while (key.endsWith(u'/'))
        key.chop(1);
    return key.mid(key.lastIndexOf(u'/') + 1);
}

constexpr int kParallelHeads = 8;

} // namespace

DeleteJob::DeleteJob(const Account& account, const QString& bucket, const QList<Item>& items,
                     const QString& planDir, QObject* parent)
    : Job(account, bucket, parent),
      m_planDir(planDir)
{
    // A key inside a selected folder is deleted with the folder: only once.
    QStringList folders;
    for (const Item& i : items) {
        if (i.folder && !i.key.isEmpty())
            folders.append(i.key);
    }
    for (const Item& i : items) {
        if (i.key.isEmpty())
            continue;
        const bool covered = std::any_of(folders.cbegin(), folders.cend(), [&](const QString& f) {
            return i.key != f && i.key.startsWith(f);
        });
        const bool duplicate = std::any_of(m_items.cbegin(), m_items.cend(),
                                           [&](const Item& seen) { return seen.key == i.key; });
        if (!covered && !duplicate)
            m_items.append(i);
    }
}

DeleteJob::~DeleteJob() = default;

QString DeleteJob::title() const
{
    if (m_items.size() == 1) {
        const Item& i = m_items.first();
        return i.folder ? tr("Delete folder “%1”").arg(lastSegment(i.key)) : tr("Delete “%1”").arg(lastSegment(i.key));
    }
    return tr("Delete %n item(s)", nullptr, int(m_items.size()));
}

// --- plan --------------------------------------------------------------------

void DeleteJob::plan()
{
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    for (const Item& i : std::as_const(m_items)) {
        if (i.folder)
            m_folders.append(i.key);
        else
            m_objects.append(i.key);
    }
    m_planIndex = 0;
    // The bucket's versioning state decides what the preview promises.
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 600, m_run.get(), [this](bool) { planNext_(); });
}

void DeleteJob::planNext_()
{
    // Single objects first, kParallelHeads at a time; then each folder, page by page.
    if (isCancelled())
        return;
    if (m_planIndex < m_objects.size()) {
        const qsizetype end = std::min<qsizetype>(m_objects.size(), m_planIndex + kParallelHeads);
        auto pending = std::make_shared<int>(int(end - m_planIndex));
        for (; m_planIndex < end; ++m_planIndex) {
            const QString key = m_objects[m_planIndex];
            s3::Session* session = session_();
            if (!session) {
                fail_(tr("Not connected."));
                return;
            }
            session->headObject(bucket(), key, QString(), m_run.get(), [this, key, pending](const s3::Result<s3::ObjectMeta>& head) {
                if (phase() != Phase::Planning)
                    return;
                if (head) {
                    m_plan->append({key});
                    m_existing.append(key);
                    m_bytes += std::max<qint64>(0, head->size);
                    if (m_samples.size() < kSamples)
                        m_samples.append(key);
                } else if (head.error().category == s3::ErrorCategory::NotFound) {
                    ++m_skipped;   // gone already
                } else {
                    fail_(tr("Could not look up “%1”: %2").arg(key, ConnectionTester::describe(head.error())));
                    return;
                }
                if (--*pending == 0)
                    planNext_();
            });
        }
        return;
    }
    const qsizetype folderIndex = m_planIndex - m_objects.size();
    if (folderIndex < m_folders.size()) {
        planFolder_(m_folders[folderIndex]);
        return;
    }
    planDone_();
}

void DeleteJob::planFolder_(const QString& prefix)
{
    if (!m_pager) {
        s3::Session* session = session_();
        if (!session) {
            fail_(tr("Not connected."));
            return;
        }
        s3::ListObjectsParams params;
        params.bucket = bucket();
        params.prefix = prefix;   // flat: everything below, the marker included
        params.maxKeys = 1000;
        m_pager = std::make_unique<s3::ListObjectsPager>(*session, params);
    }
    m_pager->fetchNext(m_run.get(), [this, prefix](const s3::Result<s3::ListPage>& page) {
        if (phase() != Phase::Planning)
            return;
        if (!page) {
            fail_(tr("Could not list “%1”: %2").arg(prefix, ConnectionTester::describe(page.error())));
            return;
        }
        QStringList keys;
        keys.reserve(page->objects.size());
        for (const s3::ObjectEntry& e : page->objects) {
            keys.append(e.key);
            m_bytes += e.size;
            if (m_samples.size() < kSamples)
                m_samples.append(e.key);
        }
        m_plan->append(keys);
        setProgress_(m_plan->count(), 0);
        if (m_pager->hasMore()) {
            planFolder_(prefix);
            return;
        }
        m_pager.reset();
        ++m_planIndex;
        planNext_();
    });
}

void DeleteJob::planDone_()
{
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        if (m_plan->count() == 0) {
            m_verification = tr("nothing to delete");
            finish_(report_());
            return;
        }
        Preview p;
        p.action = tr("Delete");
        if (m_items.size() == 1)
            p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_items.first().key);
        else
            p.scope = tr("s3://%1 — %n selected item(s)", nullptr, int(m_items.size())).arg(bucket());
        p.objects = m_plan->count();
        p.bytes = m_bytes;
        p.samples = m_samples;
        if (m_skipped > 0)
            p.details.append(tr("%n selected object(s) no longer exist and are skipped.", nullptr, int(m_skipped)));

        const std::optional<s3::CapabilitySet> state =
            controller_()->capabilities()->bucketSet(account().id, bucket());
        const QString versioning = state ? state->bucketState.versioning : QString();
        if (!state) {
            p.permanent = true;
            p.warnings.append(tr("The bucket's versioning state is unknown; assume the deletion is permanent."));
        } else if (versioning == QLatin1String("Enabled")) {
            p.warnings.append(tr("The bucket keeps versions: delete markers are created, the earlier versions stay "
                                 "and can be restored."));
        } else if (versioning == QLatin1String("Suspended")) {
            p.permanent = true;
            p.warnings.append(tr("Versioning is suspended: objects written since then are deleted for good."));
        } else {
            p.permanent = true;
        }
        if (state && state->bucketState.objectLockEnabled.value_or(false))
            p.warnings.append(tr("Object lock is enabled: locked versions are not removed, only hidden behind "
                                 "delete markers."));
        if (p.objects > OperationController::kTypeToConfirmAbove)
            p.typeToConfirm = m_items.size() == 1 && m_items.first().folder ? lastSegment(m_items.first().key)
                                                                             : bucket();
        setProgress_(0, p.objects);
        setPreview_(p);
    });
}

// --- execute -----------------------------------------------------------------

void DeleteJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    // Batches only where DeleteObjects is confirmed; one by one otherwise.
    m_useBatches = controller_()->capabilities()->decide(account().id, bucket(), s3::Feature::BatchDelete, true)
                   == s3::Decision::Allowed;
    m_batchSize = m_useBatches ? std::clamp(session->options().profile->maxDeleteBatch(), 1, 1000) : 1;
    m_plan->rewind();
    m_eof = false;
    setProgress_(0, m_plan->count());
    pump_();
}

void DeleteJob::pump_()
{
    const int parallel = m_useBatches ? kParallelBatches : kParallelSingles;
    if (isCancelled() || m_reading || m_eof || m_inFlight >= parallel) {
        settled_();
        return;
    }
    m_reading = true;
    m_plan->readNext(m_batchSize, [this](const QStringList& keys) {
        m_reading = false;
        if (phase() != Phase::Executing)
            return;
        if (keys.isEmpty()) {
            m_eof = true;
            settled_();
            return;
        }
        if (isCancelled()) {
            settled_();
            return;
        }
        if (m_useBatches)
            sendBatch_(keys);
        else
            sendSingle_(keys.first());
        pump_();
    });
}

void DeleteJob::sendBatch_(const QStringList& keys)
{
    s3::Session* session = session_();
    if (!session) {
        for (const QString& k : keys)
            addFailure_(k, tr("not connected"));
        return;
    }
    QList<s3::ObjectIdentifier> ids;
    ids.reserve(keys.size());
    for (const QString& k : keys)
        ids.append({k, QString()});
    ++m_inFlight;
    session->deleteObjects(bucket(), ids, m_run.get(), [this, keys](const s3::Result<s3::DeleteObjectsResult>& r) {
        --m_inFlight;
        Journal::Record batch = record_(s3::operationName(s3::Operation::DeleteObjects), keys.first());
        QList<Journal::Record> records;
        if (r) {
            m_succeeded += r->deleted.size();
            // One record per object: on a versioned bucket it names the
            // delete marker that now hides the object (the way back).
            for (const auto& d : r->deleted) {
                Journal::Record one = record_(s3::operationName(s3::Operation::DeleteObjects), d.key);
                one.httpStatus = 200;
                one.requestId = r->requestId;
                one.versionId = d.deleteMarker ? d.deleteMarkerVersionId : d.versionId;
                if (d.deleteMarker)
                    one.message = QStringLiteral("delete marker");
                records.append(one);
            }
            for (const auto& f : r->failed) {
                addFailure_(f.key, f.code.isEmpty() ? f.message : f.code);
                Journal::Record one = record_(s3::operationName(s3::Operation::DeleteObjects), f.key);
                one.result = Journal::Result::Failed;
                one.httpStatus = 200;
                one.requestId = r->requestId;
                one.message = QStringLiteral("%1: %2").arg(f.code, f.message);
                records.append(one);
            }
            batch.httpStatus = 200;
            batch.requestId = r->requestId;
            batch.result = r->failed.isEmpty() ? Journal::Result::Ok : Journal::Result::Failed;
            batch.message = QStringLiteral("batch of %1: %2 deleted, %3 failed")
                                .arg(keys.size()).arg(r->deleted.size()).arg(r->failed.size());
        } else {
            const s3::Error& e = r.error();
            const QString reason = e.outcomeUnknown ? tr("the answer was lost — the objects may or may not be deleted")
                                                    : ConnectionTester::describe(e);
            for (const QString& k : keys)
                addFailure_(k, reason);
            batch.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            batch.httpStatus = e.httpStatus;
            batch.requestId = e.requestId;
            batch.message = QStringLiteral("batch of %1: %2").arg(keys.size()).arg(e.toString());
        }
        records.prepend(batch);
        journal_(records);
        setProgress_(progressDone() + keys.size(), progressTotal());
        pump_();
    });
}

void DeleteJob::sendSingle_(const QString& key)
{
    s3::Session* session = session_();
    if (!session) {
        addFailure_(key, tr("not connected"));
        return;
    }
    ++m_inFlight;
    session->deleteObject(bucket(), key, QString(), m_run.get(), [this, key](const s3::Result<s3::DeleteObjectResult>& r) {
        --m_inFlight;
        Journal::Record rec = record_(s3::operationName(s3::Operation::DeleteObject), key);
        if (r) {
            ++m_succeeded;
            rec.httpStatus = 204;
            rec.requestId = r->requestId;
            rec.versionId = r->versionId;
            if (r->deleteMarker)
                rec.message = QStringLiteral("delete marker");
        } else {
            const s3::Error& e = r.error();
            addFailure_(key, ConnectionTester::describe(e));
            rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message = e.toString();
        }
        journal_({rec});
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
    });
}

void DeleteJob::settled_()
{
    if (phase() != Phase::Executing || m_inFlight > 0 || m_reading)
        return;
    if (isCancelled()) {
        m_verification = tr("cancelled — %1 of %2 deleted").arg(m_succeeded).arg(m_plan->count());
        finish_(report_());
        return;
    }
    if (m_eof)
        executed_();
}

void DeleteJob::onCancel()
{
    switch (phase()) {
    case Phase::Planning:
        m_run.reset();
        m_pager.reset();
        break;
    case Phase::Executing:
        settled_();   // ends now if nothing is in flight, else once it is back
        break;
    case Phase::Verifying:
        m_run.reset();
        m_verification = tr("deleted; cancelled while verifying");
        finish_(report_());
        break;
    default:
        break;
    }
}

// --- verify ------------------------------------------------------------------

void DeleteJob::verify()
{
    verifyPass_(0);
}

void DeleteJob::verifyPass_(int pass)
{
    verifyFolders_(0, {}, pass);
}

void DeleteJob::verifyFolders_(qsizetype index, QStringList remaining, int pass)
{
    if (index >= m_folders.size()) {
        verifyObjects_(0, std::move(remaining), pass);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not verified (not connected)");
        finish_(report_());
        return;
    }
    s3::ListObjectsParams params;
    params.bucket = bucket();
    params.prefix = m_folders[index];
    params.maxKeys = 1000;
    session->listObjects(params, m_run.get(), [this, index, remaining, pass](const s3::Result<s3::ListPage>& page) mutable {
        if (!page) {
            m_verification = tr("not verified: %1").arg(ConnectionTester::describe(page.error()));
            finish_(report_());
            return;
        }
        for (const s3::ObjectEntry& e : page->objects) {
            if (!m_failedSet.contains(e.key))
                remaining.append(e.key);
        }
        verifyFolders_(index + 1, std::move(remaining), pass);
    });
}

void DeleteJob::verifyObjects_(qsizetype index, QStringList remaining, int pass)
{
    // The keys deleted one by one: looked up again (the first kVerifyHeadLimit).
    while (index < m_existing.size() && index < kVerifyHeadLimit && m_failedSet.contains(m_existing[index]))
        ++index;
    if (index >= m_existing.size() || index >= kVerifyHeadLimit) {
        verifyResult_(remaining, pass);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not verified (not connected)");
        finish_(report_());
        return;
    }
    const QString key = m_existing[index];
    session->headObject(bucket(), key, QString(), m_run.get(), [this, index, remaining, pass, key](const s3::Result<s3::ObjectMeta>& head) mutable {
        if (head)
            remaining.append(key);
        else if (head.error().category != s3::ErrorCategory::NotFound) {
            m_verification = tr("not verified: %1").arg(ConnectionTester::describe(head.error()));
            finish_(report_());
            return;
        }
        verifyObjects_(index + 1, std::move(remaining), pass);
    });
}

void DeleteJob::verifyResult_(const QStringList& remaining, int pass)
{
    if (remaining.isEmpty()) {
        m_verified = true;
        m_verification = m_existing.size() > kVerifyHeadLimit
                             ? tr("verified (folders listed, the first %1 single objects looked up)").arg(kVerifyHeadLimit)
                             : tr("verified: nothing left");
        finish_(report_());
        return;
    }
    // Listings may lag behind a delete for a while: look again before
    // calling it a failure.
    if (pass < 2) {
        const int delay = pass == 0 ? m_recheck.first : m_recheck.second;
        QTimer::singleShot(delay, m_run.get(), [this, pass] { verifyPass_(pass + 1); });
        return;
    }
    m_succeeded = std::max<qint64>(0, m_succeeded - remaining.size());
    for (const QString& k : remaining)
        addFailure_(k, tr("still there after the delete"));
    m_verified = false;
    m_verification = tr("%n object(s) still there after deleting", nullptr, int(remaining.size()));
    finish_(report_());
}

void DeleteJob::addFailure_(const QString& key, const QString& reason)
{
    if (m_failedSet.contains(key))
        return;
    m_failedSet.insert(key);
    m_failedKeys.append(key);
    if (m_failures.size() < kMaxListed)
        m_failures.append(QStringLiteral("%1: %2").arg(key, reason));
}

Job::Report DeleteJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped;
    r.failed = m_failedKeys.size();
    r.failures = m_failures;
    r.verification = m_verification;
    r.verified = m_verified;
    return r;
}

} // namespace qbucket
