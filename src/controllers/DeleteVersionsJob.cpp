#include "controllers/DeleteVersionsJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/VersionWalk.h"

#include <QLocale>
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

QString refId(const QString& key, const QString& versionId)
{
    return key + QChar(u'\0') + versionId;
}

} // namespace

DeleteVersionsJob::DeleteVersionsJob(Kind kind, const Account& account, const QString& bucket, const QString& planDir)
    : Job(account, bucket),
      m_kind(kind),
      m_planDir(planDir)
{
}

DeleteVersionsJob::~DeleteVersionsJob() = default;

DeleteVersionsJob* DeleteVersionsJob::versions(const Account& account, const QString& bucket,
                                               const QList<VersionRef>& refs, const QString& planDir)
{
    auto* job = new DeleteVersionsJob(Kind::Versions, account, bucket, planDir);
    QSet<QString> keys;
    for (const VersionRef& r : refs) {
        if (r.key.isEmpty() || r.versionId.isEmpty() || job->m_wanted.contains(refId(r.key, r.versionId)))
            continue;
        job->m_refs.append(r);
        job->m_wanted.insert(refId(r.key, r.versionId));
        if (!keys.contains(r.key)) {
            keys.insert(r.key);
            job->m_walks.append({r.key, true});
        }
    }
    return job;
}

DeleteVersionsJob* DeleteVersionsJob::everything(const Account& account, const QString& bucket,
                                                 const QList<Item>& items, const QString& planDir)
{
    auto* job = new DeleteVersionsJob(Kind::Everything, account, bucket, planDir);
    job->addItems_(items);
    return job;
}

DeleteVersionsJob* DeleteVersionsJob::undelete(const Account& account, const QString& bucket, const QList<Item>& items,
                                               const QString& planDir, const QDateTime& deletedAfter)
{
    auto* job = new DeleteVersionsJob(Kind::Undelete, account, bucket, planDir);
    job->addItems_(items);
    job->m_deletedAfter = deletedAfter;
    return job;
}

// A key inside a selected folder is covered by the folder: walked once.
void DeleteVersionsJob::addItems_(const QList<Item>& items)
{
    QStringList folders;
    for (const Item& i : items) {
        if (i.folder && !i.key.isEmpty())
            folders.append(i.key);
    }
    for (const Item& i : items) {
        if (i.key.isEmpty())
            continue;
        const bool covered = std::any_of(folders.cbegin(), folders.cend(),
                                         [&](const QString& f) { return i.key != f && i.key.startsWith(f); });
        const bool duplicate = std::any_of(m_items.cbegin(), m_items.cend(), [&](const Item& seen) { return seen.key == i.key; });
        if (covered || duplicate)
            continue;
        m_items.append(i);
        m_walks.append({i.key, !i.folder});
    }
}

QString DeleteVersionsJob::title() const
{
    switch (m_kind) {
    case Kind::Versions:
        return tr("Delete %n version(s) permanently", nullptr, int(m_refs.size()));
    case Kind::Everything:
        return m_items.size() == 1 ? tr("Delete every version of “%1”").arg(lastSegment(m_items.first().key))
                                   : tr("Delete every version of %n item(s)", nullptr, int(m_items.size()));
    case Kind::Undelete:
        if (m_items.size() == 1) {
            return m_items.first().folder ? tr("Restore what was deleted in “%1”").arg(lastSegment(m_items.first().key))
                                          : tr("Restore “%1”").arg(lastSegment(m_items.first().key));
        }
        return tr("Restore %n deleted item(s)", nullptr, int(m_items.size()));
    }
    return {};
}

QList<s3::Feature> DeleteVersionsJob::requiredFeatures() const
{
    return {s3::Feature::ObjectVersionsListing};
}

// --- plan ----------------------------------------------------------------------

void DeleteVersionsJob::plan()
{
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    if (m_walks.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Nothing selected.")); }, Qt::QueuedConnection);
        return;
    }
    // MFA delete and the versioning state as they are now.
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 60, m_run.get(), [this](bool fresh) {
        if (!fresh) {
            fail_(tr("The bucket's versioning state could not be checked. Nothing was changed."));
            return;
        }
        checkVersioning_();
    });
}

void DeleteVersionsJob::checkVersioning_()
{
    const CapabilityController::Versioning v = controller_()->capabilities()->versioningOf(account().id, bucket());
    if (v.mfaDelete.value_or(false)) {
        fail_(tr("MFA delete is enabled on this bucket: deleting versions needs a code from the MFA device, which "
                 "QBucket can't send. Nothing was changed."));
        return;
    }
    planNext_();
}

void DeleteVersionsJob::planNext_()
{
    if (phase() != Phase::Planning)
        return;
    if (m_walkIndex >= m_walks.size()) {
        planDone_();
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    const Walk w = m_walks[m_walkIndex];
    m_walk = std::make_unique<VersionWalk>(*session, bucket(), w.prefix, w.exact, m_run.get());
    m_walk->run([this](const QList<s3::VersionEntry>& entries, bool last) { return planPage_(entries, last); },
                [this, prefix = w.prefix](const s3::Error& e) {
                    if (phase() == Phase::Planning)
                        fail_(tr("Could not list the versions of “%1”: %2").arg(prefix, ConnectionTester::describe(e)));
                });
}

bool DeleteVersionsJob::planPage_(const QList<s3::VersionEntry>& entries, bool last)
{
    if (phase() != Phase::Planning)
        return false;
    for (const s3::VersionEntry& e : entries) {
        if (e.key != m_lastKey) {
            m_lastKey = e.key;
            ++m_keys;
        }
        switch (m_kind) {
        case Kind::Versions:
            if (m_wanted.remove(refId(e.key, e.versionId)))
                addTarget_({e.key, e.versionId, e.isDeleteMarker, e.isLatest, e.size, {}});
            break;
        case Kind::Everything:
            addTarget_({e.key, e.versionId, e.isDeleteMarker, e.isLatest, e.size, {}});
            break;
        case Kind::Undelete:
            undeleteEntry_(e);
            break;
        }
    }
    setProgress_(m_plan->count(), 0);
    if (!last)
        return true;
    if (m_kind == Kind::Undelete)
        finishUndeleteKey_();
    ++m_walkIndex;
    // The walk is replaced from the event loop, not inside its own callback.
    QMetaObject::invokeMethod(this, [this] { planNext_(); }, Qt::QueuedConnection);
    return false;
}

// One key's entries arrive newest first, maybe across pages: the markers on
// top go, up to the first real version — which then becomes current.
void DeleteVersionsJob::undeleteEntry_(const s3::VersionEntry& e)
{
    if (e.key != m_curKey) {
        finishUndeleteKey_();
        m_curKey = e.key;
    }
    if (m_curDecided)
        return;
    if (e.isDeleteMarker) {
        m_curMarkers.append(e);
        return;
    }
    m_curDecided = true;
    if (m_curMarkers.isEmpty())
        return;   // not deleted
    if (m_deletedAfter.isValid() && m_curMarkers.first().lastModified < m_deletedAfter) {
        ++m_skipped;
        if (m_notes.size() < kMaxListed)
            m_notes.append(tr("%1: deleted before the chosen time").arg(e.key));
        m_curMarkers.clear();
        return;
    }
    for (const s3::VersionEntry& m : std::as_const(m_curMarkers))
        addTarget_({m.key, m.versionId, true, m.isLatest, 0, e.versionId});
    ++m_restoredKeys;
    m_bytes += e.size;
    if (m_revealed.size() < kVerifyHeadLimit)
        m_revealed.append({e.key, e.versionId});
    m_curMarkers.clear();
}

void DeleteVersionsJob::finishUndeleteKey_()
{
    if (!m_curDecided && !m_curMarkers.isEmpty()) {
        ++m_skipped;   // only markers: removing them would bring nothing back
        if (m_notes.size() < kMaxListed)
            m_notes.append(tr("%1: no version left under its delete marker").arg(m_curKey));
    }
    m_curKey.clear();
    m_curMarkers.clear();
    m_curDecided = false;
}

void DeleteVersionsJob::addTarget_(const Target& t)
{
    m_plan->appendRecords({{t.key, t.versionId, t.marker ? QStringLiteral("1") : QStringLiteral("0"),
                            t.latest ? QStringLiteral("1") : QStringLiteral("0"), QString::number(t.size),
                            t.revealsVersion}});
    if (t.marker) {
        ++m_markerCount;
    } else {
        ++m_versionCount;
        if (m_kind != Kind::Undelete)
            m_bytes += t.size;
        if (t.latest)
            ++m_currentCount;
    }
    if (m_kind == Kind::Versions)
        m_planned.insert(refId(t.key, t.versionId));
    if (m_samples.size() < kSamples) {
        m_samples.append(m_kind == Kind::Undelete || t.versionId.isEmpty()
                             ? t.key
                             : tr("%1 (version %2)").arg(t.key, t.versionId));
    }
}

DeleteVersionsJob::Target DeleteVersionsJob::targetOf(const QStringList& record)
{
    Target t;
    t.key = record.value(0);
    t.versionId = record.value(1);
    t.marker = record.value(2) == QLatin1String("1");
    t.latest = record.value(3) == QLatin1String("1");
    t.size = record.value(4).toLongLong();
    t.revealsVersion = record.value(5);
    return t;
}

void DeleteVersionsJob::planDone_()
{
    m_walk.reset();
    if (m_kind == Kind::Versions && !m_wanted.isEmpty()) {
        m_skipped += m_wanted.size();
        for (const QString& gone : std::as_const(m_wanted)) {
            if (m_notes.size() < kMaxListed)
                m_notes.append(tr("%1: this version no longer exists").arg(gone.section(QChar(u'\0'), 0, 0)));
        }
    }
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        if (m_plan->count() == 0) {
            m_verification = m_kind == Kind::Undelete ? tr("nothing deleted to restore") : tr("nothing to delete");
            finish_(report_());
            return;
        }
        const QLocale locale;
        Preview p;
        p.objects = m_plan->count();
        p.samples = m_samples;
        if (m_items.size() == 1)
            p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_items.first().key);
        else if (m_kind == Kind::Versions && m_walks.size() == 1)
            p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_walks.first().prefix);
        else
            p.scope = tr("s3://%1 — %n selected item(s)", nullptr, int(std::max(m_items.size(), m_walks.size()))).arg(bucket());
        if (m_skipped > 0)
            p.details.append(tr("%n selected item(s) are skipped — see the report.", nullptr, int(m_skipped)));
        const CapabilityController::Versioning v = controller_()->capabilities()->versioningOf(account().id, bucket());
        if (m_kind == Kind::Undelete) {
            p.action = tr("Restore");
            p.bytes = m_bytes;
            p.objects = m_restoredKeys;
            p.details.append(tr("%n object(s) come back: the delete markers over their newest version are removed "
                                "(%1 marker(s)).", nullptr, int(m_restoredKeys))
                                 .arg(locale.toString(m_markerCount)));
            p.details.append(tr("Nothing is copied or overwritten; deleting them again works as before."));
        } else {
            p.action = tr("Delete Permanently");
            p.bytes = m_bytes;
            p.permanent = true;
            p.details.append(tr("%1 version(s) and %2 delete marker(s) of %n object(s).", nullptr, int(m_keys))
                                 .arg(locale.toString(m_versionCount), locale.toString(m_markerCount)));
            if (m_kind == Kind::Everything) {
                p.warnings.append(tr("Every version goes: the objects disappear completely, nothing is left to "
                                     "restore."));
            } else if (m_currentCount > 0) {
                p.warnings.append(tr("%n current version(s) are among them: the previous version becomes current, "
                                     "or the object disappears when none is left.", nullptr, int(m_currentCount)));
            }
            if (v.objectLock.value_or(false)) {
                p.warnings.append(tr("Object lock is enabled: versions under retention or a legal hold can't be "
                                     "deleted and are reported, never bypassed."));
            }
            // Always typed: nothing of it can be undone.
            if (m_items.size() == 1)
                p.typeToConfirm = lastSegment(m_items.first().key);
            else if (m_kind == Kind::Versions && m_walks.size() == 1)
                p.typeToConfirm = lastSegment(m_walks.first().prefix);
            else
                p.typeToConfirm = bucket();
        }
        setProgress_(0, m_plan->count());
        setPreview_(p);
    });
}

// --- execute ---------------------------------------------------------------------

void DeleteVersionsJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    m_useBatches = controller_()->capabilities()->decide(account().id, bucket(), s3::Feature::BatchDelete, true)
                   == s3::Decision::Allowed;
    m_batchSize = m_useBatches ? std::clamp(session->options().profile->maxDeleteBatch(), 1, 1000) : 1;
    m_plan->rewind();
    m_eof = false;
    setProgress_(0, m_plan->count());
    pump_();
}

void DeleteVersionsJob::pump_()
{
    const int parallel = m_useBatches ? kParallelBatches : kParallelSingles;
    if (isCancelled() || m_reading || m_eof || m_inFlight >= parallel) {
        settled_();
        return;
    }
    m_reading = true;
    m_plan->readNextRecords(m_batchSize, [this](const QList<QStringList>& records) {
        m_reading = false;
        if (phase() != Phase::Executing)
            return;
        if (records.isEmpty()) {
            m_eof = true;
            settled_();
            return;
        }
        if (isCancelled()) {
            settled_();
            return;
        }
        QList<Target> targets;
        targets.reserve(records.size());
        for (const QStringList& r : records)
            targets.append(targetOf(r));
        if (m_useBatches)
            sendBatch_(targets);
        else
            sendSingle_(targets.first());
        pump_();
    });
}

void DeleteVersionsJob::sendBatch_(const QList<Target>& targets)
{
    s3::Session* session = session_();
    if (!session) {
        for (const Target& t : targets)
            failed_(t, tr("not connected"));
        return;
    }
    QList<s3::ObjectIdentifier> ids;
    ids.reserve(targets.size());
    for (const Target& t : targets)
        ids.append({t.key, t.versionId});
    ++m_inFlight;
    session->deleteObjects(bucket(), ids, m_run.get(), [this, targets](const s3::Result<s3::DeleteObjectsResult>& r) {
        --m_inFlight;
        Journal::Record batch = record_(s3::operationName(s3::Operation::DeleteObjects), targets.first().key);
        if (r) {
            QHash<QString, const s3::DeleteObjectsResult::Failed*> refused;
            for (const auto& f : r->failed)
                refused.insert(refId(f.key, f.versionId), &f);
            for (const Target& t : targets) {
                if (const auto* f = refused.value(refId(t.key, t.versionId))) {
                    Journal::Record one = record_(s3::operationName(s3::Operation::DeleteObjects), t.key);
                    one.versionId = t.versionId;
                    one.result = Journal::Result::Failed;
                    one.httpStatus = 200;
                    one.requestId = r->requestId;
                    one.message = QStringLiteral("%1: %2").arg(f->code, f->message);
                    journal_({one});
                    failed_(t, f->code == QLatin1String("AccessDenied")
                                   ? tr("protected by object lock, or not permitted (%1)").arg(f->message)
                                   : (f->code.isEmpty() ? f->message : f->code));
                } else {
                    deleted_(t, r->requestId);
                }
            }
            batch.httpStatus = 200;
            batch.requestId = r->requestId;
            batch.result = r->failed.isEmpty() ? Journal::Result::Ok : Journal::Result::Failed;
            batch.message = QStringLiteral("versions, batch of %1: %2 failed").arg(targets.size()).arg(r->failed.size());
        } else {
            const s3::Error& e = r.error();
            const QString reason = e.outcomeUnknown ? tr("the answer was lost — they may or may not be deleted")
                                                    : ConnectionTester::describe(e);
            for (const Target& t : targets)
                failed_(t, reason);
            batch.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            batch.httpStatus = e.httpStatus;
            batch.requestId = e.requestId;
            batch.message = QStringLiteral("versions, batch of %1: %2").arg(targets.size()).arg(e.toString());
        }
        journal_({batch});
        setProgress_(progressDone() + targets.size(), progressTotal());
        pump_();
    });
}

void DeleteVersionsJob::sendSingle_(const Target& t)
{
    s3::Session* session = session_();
    if (!session) {
        failed_(t, tr("not connected"));
        return;
    }
    ++m_inFlight;
    session->deleteObject(bucket(), t.key, t.versionId, m_run.get(), [this, t](const s3::Result<s3::DeleteObjectResult>& r) {
        --m_inFlight;
        if (r) {
            deleted_(t, r->requestId);
        } else {
            const s3::Error& e = r.error();
            Journal::Record rec = record_(s3::operationName(s3::Operation::DeleteObject), t.key);
            rec.versionId = t.versionId;
            rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message = e.toString();
            journal_({rec});
            failed_(t, e.category == s3::ErrorCategory::NotPermitted
                           ? tr("protected by object lock, or not permitted (%1)").arg(ConnectionTester::describe(e))
                           : ConnectionTester::describe(e));
        }
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
    });
}

void DeleteVersionsJob::deleted_(const Target& t, const QString& requestId)
{
    ++m_succeeded;
    Journal::Record rec = record_(s3::operationName(m_useBatches ? s3::Operation::DeleteObjects : s3::Operation::DeleteObject),
                                  t.key);
    rec.versionId = t.versionId;
    rec.httpStatus = m_useBatches ? 200 : 204;
    rec.requestId = requestId;
    rec.message = t.marker ? (m_kind == Kind::Undelete ? QStringLiteral("delete marker removed (undelete)")
                                                       : QStringLiteral("delete marker removed"))
                           : QStringLiteral("version deleted permanently");
    journal_({rec});
}

void DeleteVersionsJob::failed_(const Target& t, const QString& reason)
{
    m_failedSet.insert(refId(t.key, t.versionId));
    m_failedKeys.insert(t.key);
    if (m_failures.size() < kMaxListed)
        m_failures.append(QStringLiteral("%1 (%2): %3").arg(t.key, t.versionId, reason));
}

void DeleteVersionsJob::settled_()
{
    if (phase() != Phase::Executing || m_inFlight > 0 || m_reading)
        return;
    if (isCancelled()) {
        m_verification = tr("cancelled — what was sent is in the journal");
        finish_(report_());
        return;
    }
    if (m_eof)
        executed_();
}

void DeleteVersionsJob::onCancel()
{
    if (phase() == Phase::Planning) {
        m_walk.reset();
        m_run.reset();
    } else if (phase() == Phase::Executing) {
        settled_();
    }
}

// --- verify -------------------------------------------------------------------

void DeleteVersionsJob::verify()
{
    verifyPass_(false);
}

void DeleteVersionsJob::verifyPass_(bool recheck)
{
    m_verifyIndex = 0;
    m_remaining = 0;
    m_remainingSamples.clear();
    if (m_kind == Kind::Undelete)
        verifyNextHead_(recheck);
    else
        verifyNextWalk_(recheck);
}

// Versions / Everything: what was deleted must not be listed any more.
void DeleteVersionsJob::verifyNextWalk_(bool recheck)
{
    if (m_verifyIndex >= m_walks.size()) {
        verifyDone_(recheck);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not checked (not connected)");
        finish_(report_());
        return;
    }
    const Walk w = m_walks[m_verifyIndex];
    m_walk = std::make_unique<VersionWalk>(*session, bucket(), w.prefix, w.exact, m_run.get());
    m_walk->run(
        [this, recheck](const QList<s3::VersionEntry>& entries, bool last) {
            for (const s3::VersionEntry& e : entries) {
                const QString id = refId(e.key, e.versionId);
                if (m_failedSet.contains(id))
                    continue;   // reported already
                if (m_kind == Kind::Versions && !m_planned.contains(id))
                    continue;   // not ours to delete
                ++m_remaining;
                if (m_remainingSamples.size() < kSamples)
                    m_remainingSamples.append(e.key);
            }
            if (!last)
                return true;
            ++m_verifyIndex;
            QMetaObject::invokeMethod(this, [this, recheck] { verifyNextWalk_(recheck); }, Qt::QueuedConnection);
            return false;
        },
        [this](const s3::Error& e) {
            m_verification = tr("not checked: %1").arg(ConnectionTester::describe(e));
            finish_(report_());
        });
}

// Undelete: each object is there again, as the version that was revealed.
void DeleteVersionsJob::verifyNextHead_(bool recheck)
{
    if (m_verifyIndex >= m_revealed.size()) {
        verifyDone_(recheck);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not checked (not connected)");
        finish_(report_());
        return;
    }
    const auto [key, version] = m_revealed[m_verifyIndex];
    ++m_verifyIndex;
    if (m_failedKeys.contains(key)) {
        verifyNextHead_(recheck);
        return;
    }
    session->headObject(bucket(), key, QString(), m_run.get(), [this, recheck, key, version](const s3::Result<s3::ObjectMeta>& r) {
        // RGW reports no version id on some answers: then being there counts.
        const bool back = r && (r->versionId.isEmpty() || version.isEmpty() || r->versionId == version);
        if (!back) {
            ++m_remaining;
            if (m_remainingSamples.size() < kSamples)
                m_remainingSamples.append(key);
        }
        verifyNextHead_(recheck);
    });
}

void DeleteVersionsJob::verifyDone_(bool recheck)
{
    m_walk.reset();
    if (m_remaining > 0 && !recheck) {
        // Listings and reads may lag behind the deletes for a moment.
        QTimer::singleShot(m_recheckMs, m_run.get(), [this] { verifyPass_(true); });
        return;
    }
    const QString samples = m_remainingSamples.join(QStringLiteral(", "));
    if (m_kind == Kind::Undelete) {
        m_verified = m_remaining == 0;
        m_verification = m_verified
            ? tr("%n object(s) looked up: they are back", nullptr, int(std::min<qsizetype>(m_revealed.size(), kVerifyHeadLimit)))
            : tr("%n object(s) are still not back: %1", nullptr, int(m_remaining)).arg(samples);
    } else {
        m_verified = m_remaining == 0;
        m_verification = m_verified ? tr("listed again: nothing of it is left")
                                    : tr("%n version(s) are still listed: %1", nullptr, int(m_remaining)).arg(samples);
    }
    finish_(report_());
}

Job::Report DeleteVersionsJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped;
    r.failed = m_failedSet.size();
    r.failures = m_failures;
    r.notes = m_notes;
    r.verified = m_verified;
    r.verification = m_verification;
    return r;
}

} // namespace qbucket
