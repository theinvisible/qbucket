#include "controllers/RestoreVersionsJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/InPlaceCopy.h"
#include "controllers/ObjectSnapshot.h"
#include "controllers/OperationController.h"
#include "controllers/VersionWalk.h"

namespace qbucket {

namespace {

QString refId(const QString& key, const QString& versionId)
{
    return key + QChar(u'\0') + versionId;
}

QString lastSegment(const QString& key)
{
    return key.mid(key.lastIndexOf(u'/') + 1);
}

} // namespace

RestoreVersionsJob::RestoreVersionsJob(const Account& account, const QString& bucket, const QList<VersionRef>& refs,
                                       const QString& planDir, QObject* parent)
    : Job(account, bucket, parent),
      m_planDir(planDir),
      m_copy(new InPlaceCopy(this))
{
    QSet<QString> keys;
    for (const VersionRef& r : refs) {
        if (r.key.isEmpty() || r.versionId.isEmpty() || m_wanted.contains(refId(r.key, r.versionId)))
            continue;
        m_refs.append(r);
        m_wanted.insert(refId(r.key, r.versionId));
        if (!keys.contains(r.key)) {
            keys.insert(r.key);
            m_keys.append(r.key);
        }
    }
}

RestoreVersionsJob::~RestoreVersionsJob() = default;

void RestoreVersionsJob::setRecheckDelayForTesting(int ms)
{
    m_copy->setRecheckDelay(ms);
}

QString RestoreVersionsJob::title() const
{
    if (m_refs.size() == 1)
        return tr("Restore an earlier version of “%1”").arg(lastSegment(m_refs.first().key));
    return tr("Restore %n earlier version(s)", nullptr, int(m_refs.size()));
}

QList<s3::Feature> RestoreVersionsJob::requiredFeatures() const
{
    if (m_large)
        return {s3::Feature::ObjectVersionsListing, s3::Feature::MultipartUpload};
    return {s3::Feature::ObjectVersionsListing};
}

// --- plan ------------------------------------------------------------------------

void RestoreVersionsJob::plan()
{
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    if (m_refs.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Nothing selected.")); }, Qt::QueuedConnection);
        return;
    }
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 600, m_run.get(),
                                               [this](bool) { checkCapabilities_(); });
}

// As for an edit: tags and ACLs a copy would lose must be readable.
void RestoreVersionsJob::checkCapabilities_()
{
    CapabilityController* caps = controller_()->capabilities();
    const auto state = [&](s3::Feature f) { return caps->entry(account().id, bucket(), f).state; };
    if (state(s3::Feature::ObjectTagging) == s3::CapabilityState::NotPermitted
        || state(s3::Feature::ObjectAcl) == s3::CapabilityState::NotPermitted) {
        fail_(tr("These credentials may not read the versions' tags or permissions: a restore could lose them. "
                 "Nothing was changed."));
        return;
    }
    m_readTags = state(s3::Feature::ObjectTagging) != s3::CapabilityState::Unsupported;
    m_readAcl = state(s3::Feature::ObjectAcl) != s3::CapabilityState::Unsupported;
    m_writeTags = caps->decide(account().id, bucket(), s3::Feature::ObjectTaggingWrite, true) == s3::Decision::Allowed;
    m_writeAcl = caps->decide(account().id, bucket(), s3::Feature::ObjectAclWrite, true) == s3::Decision::Allowed;
    planNext_();
}

void RestoreVersionsJob::planNext_()
{
    if (phase() != Phase::Planning)
        return;
    if (m_keyIndex >= m_keys.size()) {
        planDone_();
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    const QString key = m_keys[m_keyIndex];
    m_walk = std::make_unique<VersionWalk>(*session, bucket(), key, true, m_run.get());
    m_walk->run([this](const QList<s3::VersionEntry>& entries, bool last) { return planPage_(entries, last); },
                [this, key](const s3::Error& e) {
                    if (phase() == Phase::Planning)
                        fail_(tr("Could not list the versions of “%1”: %2").arg(key, ConnectionTester::describe(e)));
                });
}

bool RestoreVersionsJob::planPage_(const QList<s3::VersionEntry>& entries, bool last)
{
    if (phase() != Phase::Planning)
        return false;
    for (const s3::VersionEntry& e : entries) {
        if (!m_wanted.remove(refId(e.key, e.versionId)))
            continue;
        if (e.isDeleteMarker) {
            skipped_(e.key, tr("a delete marker has nothing to restore — remove the marker instead"));
            continue;
        }
        if (e.isLatest) {
            skipped_(e.key, tr("this is the current version already"));
            continue;
        }
        m_large = m_large || e.size > InPlaceCopy::kCopyLimit;
        m_bytes += e.size;
        if (m_samples.size() < kSamples)
            m_samples.append(tr("%1 (version %2)").arg(e.key, e.versionId));
        m_plan->appendRecords({{e.key, e.versionId, QString::number(e.size), e.etag}});
    }
    if (!last)
        return true;
    ++m_keyIndex;
    QMetaObject::invokeMethod(this, [this] { planNext_(); }, Qt::QueuedConnection);
    return false;
}

void RestoreVersionsJob::planDone_()
{
    m_walk.reset();
    for (const QString& gone : std::as_const(m_wanted))
        skipped_(gone.section(QChar(u'\0'), 0, 0), tr("this version no longer exists"));
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        if (m_plan->count() == 0) {
            finish_(report_());
            return;
        }
        Preview p;
        p.action = tr("Restore");
        p.scope = m_keys.size() == 1 ? QStringLiteral("s3://%1/%2").arg(bucket(), m_keys.first())
                                     : tr("%n object(s) in s3://%1", nullptr, int(m_keys.size())).arg(bucket());
        p.objects = m_plan->count();
        p.bytes = m_bytes;
        p.samples = m_samples;
        p.details.append(tr("Each version is copied over its object's current version, with its headers, tags and "
                            "permissions; everything is checked afterwards."));
        if (m_skipped > 0)
            p.details.append(tr("%n selected version(s) are skipped — see the report.", nullptr, int(m_skipped)));
        const CapabilityController::Versioning v = controller_()->capabilities()->versioningOf(account().id, bucket());
        if (v.known && v.status == s3::VersioningStatus::Enabled) {
            p.details.append(tr("The bucket keeps versions: the current versions stay as earlier ones."));
        } else {
            p.permanent = true;
            p.warnings.append(tr("The bucket doesn't keep versions now: the current versions are overwritten."));
        }
        if (m_readTags && !m_writeTags)
            p.warnings.append(tr("Versions with tags are skipped: writing tags isn't confirmed for this server."));
        if (m_readAcl && !m_writeAcl)
            p.warnings.append(tr("Public versions are skipped: changing ACLs isn't confirmed for this server."));
        setProgress_(0, p.objects);
        setPreview_(p);
    });
}

// --- execute ------------------------------------------------------------------

void RestoreVersionsJob::execute()
{
    m_plan->rewind();
    m_eof = false;
    setProgress_(0, m_plan->count());
    pump_();
}

void RestoreVersionsJob::pump_()
{
    if (isCancelled() || m_reading || m_eof || m_inFlight >= kParallel) {
        settled_();
        return;
    }
    m_reading = true;
    m_plan->readNextRecords(1, [this](const QList<QStringList>& records) {
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
        const QStringList& r = records.first();
        ++m_inFlight;
        start_({r.value(0), r.value(1), r.value(2).toLongLong(), r.value(3)});
        pump_();
    });
}

void RestoreVersionsJob::start_(const Entry& e)
{
    s3::Session* session = session_();
    if (!session) {
        done_(e, tr("not restored (not connected)"));
        return;
    }
    ObjectSnapshot::read(session, bucket(), e.key, m_readTags, m_readAcl, m_run.get(), [this, e](s3::Result<ObjectSnapshot> snap) {
        if (!snap) {
            if (snap.error().category == s3::ErrorCategory::NotFound) {
                --m_inFlight;
                skipped_(e.key, tr("the version was deleted since it was planned"));
                return;
            }
            done_(e, tr("not restored: the version could not be read completely (%1)")
                         .arg(ConnectionTester::describe(snap.error())));
            return;
        }
        // The version itself can't change; an ETag that does is not it.
        if (snap->meta.etag != e.etag) {
            done_(e, tr("not restored: the version answered with another ETag"));
            return;
        }
        if (const QString why = InPlaceCopy::cannotCarry(*snap, account().providerId, m_writeTags, m_writeAcl, false);
            !why.isEmpty()) {
            --m_inFlight;
            skipped_(e.key, why);
            return;
        }
        InPlaceCopy::Request req;
        req.key = e.key;
        req.sourceVersionId = e.versionId;
        req.source = *snap;
        req.headers = s3::objectHeadersOf(snap->meta);
        req.targetClass = snap->meta.storageClass;
        req.journalNote = QStringLiteral("restore version %1").arg(e.versionId);
        if (snap->acl) {
            const QString canned = s3::cannedAclOf(*snap->acl).value_or(QString());
            if (canned != QLatin1String("private"))
                req.canned = canned;
        }
        m_copy->run(req, [this, e](const QString& failure) { done_(e, failure); });
    }, e.versionId);
}

void RestoreVersionsJob::done_(const Entry& e, const QString& failure)
{
    --m_inFlight;
    if (failure.isEmpty()) {
        ++m_succeeded;
    } else {
        ++m_failed;
        if (m_failures.size() < kMaxListed)
            m_failures.append(QStringLiteral("%1 (%2): %3").arg(e.key, e.versionId, failure));
    }
    setProgress_(progressDone() + 1, progressTotal());
    pump_();
}

void RestoreVersionsJob::skipped_(const QString& key, const QString& why)
{
    ++m_skipped;
    if (m_notes.size() < kMaxListed)
        m_notes.append(QStringLiteral("%1: %2").arg(key, why));
    if (phase() == Phase::Executing) {
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
    }
}

void RestoreVersionsJob::settled_()
{
    if (phase() != Phase::Executing || m_inFlight > 0 || m_reading)
        return;
    if (isCancelled()) {
        finish_(report_());
        return;
    }
    if (m_eof)
        executed_();
}

void RestoreVersionsJob::onCancel()
{
    switch (phase()) {
    case Phase::Planning:
        m_walk.reset();
        m_run.reset();
        break;
    case Phase::Executing:
        m_copy->cancel();
        settled_();
        break;
    default:
        break;
    }
}

// Every restored object was checked on its own (InPlaceCopy).
void RestoreVersionsJob::verify()
{
    finish_(report_());
}

Job::Report RestoreVersionsJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped;
    r.failed = m_failed;
    r.failures = m_failures;
    r.notes = m_notes;
    r.verified = m_failed == 0 && m_succeeded > 0;
    r.verification = m_succeeded > 0 ? tr("every restored object checked (content, headers, class, tags, permissions)")
                                     : QString();
    return r;
}

} // namespace qbucket
