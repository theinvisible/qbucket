#include "controllers/RenameJob.h"

#include "controllers/TransferService.h"

#include <qbucket/s3/transfer/Copier.h>

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/CreateFolderJob.h"
#include "controllers/OperationController.h"

#include <QLocale>
#include <QTimer>

namespace qbucket {

namespace {

QString lastSegment(QString key)
{
    while (key.endsWith(u'/'))
        key.chop(1);
    return key.mid(key.lastIndexOf(u'/') + 1);
}

QString parentOf(const QString& key)
{
    QString k = key;
    while (k.endsWith(u'/'))
        k.chop(1);
    const qsizetype slash = k.lastIndexOf(u'/');
    return slash < 0 ? QString() : k.left(slash + 1);
}

// Single-part, plainly encrypted objects: the ETag is the content's MD5 and
// survives a copy unchanged.
bool etagIsContentHash(const QString& etag)
{
    return !etag.isEmpty() && !etag.contains(u'-');
}

} // namespace

RenameJob::RenameJob(const Account& account, const QString& bucket, const QString& key, bool folder,
                     const QString& newName, const QString& planDir, QObject* parent)
    : Job(account, bucket, parent),
      m_folder(folder),
      m_name(newName),
      m_source(key),
      m_target(parentOf(key) + newName + (folder ? QStringLiteral("/") : QString())),
      m_planDir(planDir)
{
}

RenameJob::~RenameJob() = default;

QString RenameJob::title() const
{
    return tr("Rename “%1” to “%2”").arg(lastSegment(m_source), m_name);
}

QString RenameJob::targetFor_(const QString& key) const
{
    return m_target + key.mid(m_source.size());
}

// --- plan --------------------------------------------------------------------

void RenameJob::plan()
{
    if (const QString problem = CreateFolderJob::validateName(m_name); !problem.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, problem] { fail_(problem); }, Qt::QueuedConnection);
        return;
    }
    if (m_name == lastSegment(m_source)) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("The name is unchanged.")); }, Qt::QueuedConnection);
        return;
    }
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 600, m_run.get(), [this](bool) { checkTarget_(); });
}

void RenameJob::checkTarget_()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    const auto exists = [this] {
        fail_(tr("“%1” exists already; renaming never overwrites anything.").arg(m_target));
    };
    if (!m_folder) {
        session->headObject(bucket(), m_target, QString(), m_run.get(), [this, exists](const s3::Result<s3::ObjectMeta>& head) {
            if (head) {
                exists();
                return;
            }
            if (head.error().category != s3::ErrorCategory::NotFound) {
                fail_(ConnectionTester::describe(head.error()));
                return;
            }
            planSource_();
        });
        return;
    }
    s3::ListObjectsParams params;
    params.bucket = bucket();
    params.prefix = m_target;
    params.maxKeys = 1;
    session->listObjects(params, m_run.get(), [this, exists](const s3::Result<s3::ListPage>& page) {
        if (!page) {
            fail_(ConnectionTester::describe(page.error()));
            return;
        }
        if (!page->objects.isEmpty() || !page->commonPrefixes.isEmpty()) {
            exists();
            return;
        }
        planSource_();
    });
}

void RenameJob::planSource_()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    if (m_folder) {
        s3::ListObjectsParams params;
        params.bucket = bucket();
        params.prefix = m_source;   // flat: everything below, the marker included
        params.maxKeys = 1000;
        m_pager = std::make_unique<s3::ListObjectsPager>(*session, params);
        planFolderPage_();
        return;
    }
    session->headObject(bucket(), m_source, QString(), m_run.get(), [this](const s3::Result<s3::ObjectMeta>& head) {
        if (!head) {
            fail_(tr("Could not look up “%1”: %2").arg(m_source, ConnectionTester::describe(head.error())));
            return;
        }
        m_large = head->size > kCopyLimit;
        m_plan->appendRecords({{m_source, QString::number(head->size), head->etag, head->storageClass}});
        m_bytes = head->size;
        m_samples = {m_source};
        planDone_();
    });
}

void RenameJob::planFolderPage_()
{
    m_pager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
        if (phase() != Phase::Planning)
            return;
        if (!page) {
            fail_(tr("Could not list “%1”: %2").arg(m_source, ConnectionTester::describe(page.error())));
            return;
        }
        QList<QStringList> records;
        for (const s3::ObjectEntry& e : page->objects) {
            m_large = m_large || e.size > kCopyLimit;
            records.append({e.key, QString::number(e.size), e.etag, e.storageClass});
            m_bytes += e.size;
            if (m_samples.size() < kSamples)
                m_samples.append(e.key);
        }
        m_plan->appendRecords(records);
        setProgress_(m_plan->count(), 0);
        if (m_pager->hasMore()) {
            planFolderPage_();
            return;
        }
        m_pager.reset();
        planDone_();
    });
}

void RenameJob::planDone_()
{
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        if (m_plan->count() == 0) {
            m_verification = tr("nothing to rename");
            finish_(report_());
            return;
        }
        Preview p;
        p.action = tr("Rename");
        p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_source);
        p.objects = m_plan->count();
        p.bytes = m_bytes;
        p.samples = m_samples;
        p.details.append(tr("New name: s3://%1/%2").arg(bucket(), m_target));
        p.details.append(tr("Each object is copied, the copy checked, and only then the original deleted."));
        const std::optional<s3::CapabilitySet> state =
            controller_()->capabilities()->bucketSet(account().id, bucket());
        if (state && state->bucketState.versioning == QLatin1String("Enabled"))
            p.warnings.append(tr("The bucket keeps versions: the old names keep their earlier versions, hidden "
                                 "behind delete markers."));
        p.warnings.append(tr("Object ACLs are not copied: the renamed objects get the bucket's default access."));
        setProgress_(0, p.objects);
        setPreview_(p);
    });
}

// --- execute -----------------------------------------------------------------

void RenameJob::execute()
{
    m_plan->rewind();
    m_eof = false;
    setProgress_(0, m_plan->count());
    pump_();
}

void RenameJob::pump_()
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
        Entry e;
        e.key = r.value(0);
        e.size = r.value(1).toLongLong();
        e.etag = r.value(2);
        e.storageClass = r.value(3);
        renameOne_(e);
        pump_();
    });
}

void RenameJob::renameOne_(const Entry& e)
{
    s3::Session* session = session_();
    if (!session) {
        addFailure_(e.key, tr("not connected"));
        return;
    }
    const QString target = targetFor_(e.key);
    if (e.size > kCopyLimit) {
        copyInParts_(e, target);
        return;
    }
    s3::CopyObjectParams params;
    params.sourceBucket = bucket();
    params.sourceKey = e.key;
    params.bucket = bucket();
    params.key = target;
    // A concurrent change of the source aborts the copy instead of
    // renaming something else than what was planned.
    params.ifMatch = e.etag;
    // Without it a copy lands in STANDARD.
    if (!e.storageClass.isEmpty() && e.storageClass != QLatin1String("STANDARD"))
        params.storageClass = e.storageClass;
    ++m_inFlight;
    session->copyObject(params, m_run.get(), [this, e, target](const s3::Result<s3::CopyObjectResult>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::CopyObject), target);
        rec.message = QStringLiteral("from %1").arg(e.key);
        if (!r) {
            --m_inFlight;
            const s3::Error& err = r.error();
            rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.requestId = err.requestId;
            rec.message += QStringLiteral(": ") + err.toString();
            journal_({rec});
            addFailure_(e.key, err.category == s3::ErrorCategory::PreconditionFailed
                                   ? tr("changed since it was planned — not renamed")
                                   : tr("not copied (%1) — the original is untouched").arg(ConnectionTester::describe(err)));
            setProgress_(progressDone() + 1, progressTotal());
            pump_();
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        rec.versionId = r->versionId;
        journal_({rec});
        copied_(e, target, false);
    });
}

QList<s3::Feature> RenameJob::requiredFeatures() const
{
    if (m_large)
        return {s3::Feature::MultipartUpload};
    return {};
}

// Above CopyObject's limit: UploadPartCopy in ranges, every part with the
// source's If-Match.
void RenameJob::copyInParts_(const Entry& e, const QString& target)
{
    s3::Session* session = session_();
    TransferService* transfers = controller_()->transfers();
    if (!session || !transfers) {
        addFailure_(e.key, tr("not copied (not connected) — the original is untouched"));
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
        return;
    }
    s3::transfer::CopyRequest req;
    req.sourceBucket = bucket();
    req.sourceKey = e.key;
    req.bucket = bucket();
    req.key = target;
    req.ifMatch = e.etag;
    if (!e.storageClass.isEmpty() && e.storageClass != QLatin1String("STANDARD"))
        req.storageClass = e.storageClass;
    ++m_inFlight;
    auto copier = std::make_unique<s3::transfer::Copier>(session, req, transfers->limiter(account().id));
    s3::transfer::Copier* raw = copier.get();
    connect(raw, &s3::transfer::Copier::finished, this, [this, raw, e, target] {
        const s3::Result<s3::transfer::CopyResult>& r = raw->result();
        Journal::Record rec = record_(QStringLiteral("MultipartCopy"), target);
        rec.message = QStringLiteral("from %1").arg(e.key);
        if (r) {
            rec.httpStatus = 200;
            rec.versionId = r->versionId;
            rec.message += QStringLiteral(", %1 parts").arg(r->parts);
            journal_({rec});
            copied_(e, target, true);
        } else {
            --m_inFlight;
            const s3::Error& err = r.error();
            rec.result = Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.message += QStringLiteral(": ") + err.toString();
            journal_({rec});
            if (err.category != s3::ErrorCategory::Cancelled) {
                addFailure_(e.key, err.category == s3::ErrorCategory::PreconditionFailed
                                       ? tr("changed since it was planned — not renamed")
                                       : tr("not copied (%1) — the original is untouched").arg(ConnectionTester::describe(err)));
            }
            setProgress_(progressDone() + 1, progressTotal());
            pump_();
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

void RenameJob::copied_(const Entry& e, const QString& target, bool inParts)
{
    s3::Session* session = session_();
    if (!session) {
        --m_inFlight;
        addFailure_(e.key, tr("copied, but not verified (not connected) — the original is kept"));
        pump_();
        return;
    }
    // Verify the copy before anything is deleted.
    session->headObject(bucket(), target, QString(), m_run.get(), [this, e, target, inParts](const s3::Result<s3::ObjectMeta>& head) {
        const bool sizeOk = head && head->size == e.size;
        // A copy in parts has an ETag of its own; the Copier guarded each
        // part with the source's ETag and checked the size.
        const bool etagOk = inParts || !etagIsContentHash(e.etag) || (head && head->etag == e.etag);
        if (!sizeOk || !etagOk) {
            --m_inFlight;
            addFailure_(e.key, head ? tr("the copy does not match (size or checksum) — the original is kept, check "
                                         "“%1”").arg(target)
                                    : tr("the copy could not be read back — the original is kept"));
            setProgress_(progressDone() + 1, progressTotal());
            pump_();
            return;
        }
        s3::Session* now = session_();
        if (!now) {
            --m_inFlight;
            addFailure_(e.key, tr("copied and verified, but the original could not be deleted (not connected)"));
            pump_();
            return;
        }
        now->deleteObject(bucket(), e.key, QString(), m_run.get(), [this, e](const s3::Result<s3::DeleteObjectResult>& r) {
            --m_inFlight;
            Journal::Record rec = record_(s3::operationName(s3::Operation::DeleteObject), e.key);
            if (r) {
                ++m_succeeded;
                rec.httpStatus = 204;
                rec.requestId = r->requestId;
                rec.versionId = r->versionId;
            } else {
                const s3::Error& err = r.error();
                rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
                rec.httpStatus = err.httpStatus;
                rec.requestId = err.requestId;
                rec.message = err.toString();
                addFailure_(e.key, tr("copied, but the original could not be deleted: %1")
                                       .arg(ConnectionTester::describe(err)));
            }
            journal_({rec});
            setProgress_(progressDone() + 1, progressTotal());
            pump_();
        });
    });
}

void RenameJob::settled_()
{
    if (phase() != Phase::Executing || m_inFlight > 0 || m_reading)
        return;
    if (isCancelled()) {
        m_verification = tr("cancelled — %1 of %2 renamed").arg(m_succeeded).arg(m_plan->count());
        finish_(report_());
        return;
    }
    if (m_eof)
        executed_();
}

void RenameJob::onCancel()
{
    switch (phase()) {
    case Phase::Planning:
        m_run.reset();
        m_pager.reset();
        break;
    case Phase::Executing:
        for (const auto& c : m_copiers)
            c->cancel();   // aborts its upload; the source stays
        settled_();
        break;
    case Phase::Verifying:
        m_run.reset();
        m_verification = tr("renamed; cancelled while verifying");
        finish_(report_());
        break;
    default:
        break;
    }
}

// --- verify ------------------------------------------------------------------

void RenameJob::verify()
{
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not verified (not connected)");
        finish_(report_());
        return;
    }
    if (!m_folder) {
        session->headObject(bucket(), m_source, QString(), m_run.get(), [this](const s3::Result<s3::ObjectMeta>& oldName) {
            const bool gone = !oldName && oldName.error().category == s3::ErrorCategory::NotFound;
            m_verified = m_succeeded == 1 && gone;
            m_verification = m_verified ? tr("verified") : (m_succeeded == 1 ? tr("not verified: the old name still answers")
                                                                             : QString());
            finish_(report_());
        });
        return;
    }
    // The old folder holds only what failed; the new one what was renamed.
    s3::ListObjectsParams params;
    params.bucket = bucket();
    params.prefix = m_source;
    params.maxKeys = 1000;
    session->listObjects(params, m_run.get(), [this, params](const s3::Result<s3::ListPage>& oldPage) mutable {
        if (!oldPage) {
            m_verification = tr("not verified: %1").arg(ConnectionTester::describe(oldPage.error()));
            finish_(report_());
            return;
        }
        qint64 leftovers = 0;
        for (const s3::ObjectEntry& e : oldPage->objects)
            leftovers += m_failedSet.contains(e.key) ? 0 : 1;
        s3::Session* now = session_();
        if (!now) {
            m_verification = tr("not verified (not connected)");
            finish_(report_());
            return;
        }
        params.prefix = m_target;
        now->listObjects(params, m_run.get(), [this, leftovers](const s3::Result<s3::ListPage>& newPage) {
            if (!newPage) {
                m_verification = tr("not verified: %1").arg(ConnectionTester::describe(newPage.error()));
                finish_(report_());
                return;
            }
            const qint64 expected = std::min<qint64>(m_succeeded, 1000);
            const bool targetOk = newPage->isTruncated || newPage->objects.size() >= expected;
            m_verified = leftovers == 0 && targetOk;
            if (m_verified)
                m_verification = tr("verified");
            else if (leftovers > 0)
                m_verification = tr("not verified: %n object(s) still under the old name", nullptr, int(leftovers));
            else
                m_verification = tr("not verified: fewer objects under the new name than renamed");
            finish_(report_());
        });
    });
}

void RenameJob::addFailure_(const QString& key, const QString& reason)
{
    if (m_failedSet.contains(key))
        return;
    m_failedSet.insert(key);
    m_failedKeys.append(key);
    if (m_failures.size() < kMaxListed)
        m_failures.append(QStringLiteral("%1: %2").arg(key, reason));
}

Job::Report RenameJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.failed = m_failedKeys.size();
    r.failures = m_failures;
    r.verification = m_verification;
    r.verified = m_verified;
    return r;
}

} // namespace qbucket
