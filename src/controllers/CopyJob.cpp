#include "controllers/CopyJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/ObjectCopyRunner.h"
#include "controllers/OperationController.h"
#include "controllers/UploadJob.h"

#include <QLocale>
#include <QTimeZone>
#include <QTimer>

namespace qbucket {

namespace {

QString describeSide(qint64 size, qint64 mtimeMs)
{
    const QLocale locale;
    const QString when = mtimeMs > 0
        ? locale.toString(QDateTime::fromMSecsSinceEpoch(mtimeMs, QTimeZone::UTC).toLocalTime(), QLocale::ShortFormat)
        : QStringLiteral("?");
    return QStringLiteral("%1, %2").arg(locale.formattedDataSize(std::max<qint64>(0, size)), when);
}

bool isMarker(const QString& key)
{
    return key.endsWith(u'/');
}

bool keepsVersions(OperationController* ops, const QUuid& account, const QString& bucket)
{
    const std::optional<s3::CapabilitySet> state = ops->capabilities()->bucketSet(account, bucket);
    return state && state->bucketState.versioning == QLatin1String("Enabled");
}

} // namespace

QStringList CopyJob::Record::fields() const
{
    return {key, QString::number(size), etag, storageClass, target, exists ? QStringLiteral("1") : QStringLiteral("0"),
            QString::number(targetSize), QString::number(targetMtimeMs), QString::number(mtimeMs)};
}

CopyJob::Record CopyJob::Record::from(const QStringList& f)
{
    Record r;
    r.key = f.value(0);
    r.size = f.value(1).toLongLong();
    r.etag = f.value(2);
    r.storageClass = f.value(3);
    r.target = f.value(4);
    r.exists = f.value(5) == QLatin1String("1");
    r.targetSize = f.value(6).toLongLong();
    r.targetMtimeMs = f.value(7).toLongLong();
    r.mtimeMs = f.value(8).toLongLong();
    return r;
}

CopyJob::CopyJob(const Account& account, const QString& sourceBucket, const QList<Item>& items,
                 const QString& sourceBase, const QString& targetBucket, const QString& targetPrefix, Mode mode,
                 const QString& planDir, QObject* parent)
    : Job(account, sourceBucket, parent),
      m_targetBucket(targetBucket),
      m_items(items),
      m_sourceBase(sourceBase),
      m_targetPrefix(targetPrefix),
      m_mode(mode),
      m_planDir(planDir),
      m_runner(new ObjectCopyRunner(this))
{
}

CopyJob::~CopyJob() = default;

QString CopyJob::title() const
{
    const QString target = QStringLiteral("s3://%1/%2").arg(m_targetBucket, m_targetPrefix);
    return m_mode == Mode::Move ? tr("Move %n item(s) to “%1”", nullptr, int(m_items.size())).arg(target)
                                : tr("Copy %n item(s) to “%1”", nullptr, int(m_items.size())).arg(target);
}

QStringList CopyJob::buckets() const
{
    return sameBucket_() ? QStringList{bucket()} : QStringList{bucket(), m_targetBucket};
}

QList<s3::Feature> CopyJob::requiredFeatures() const
{
    QList<s3::Feature> out;
    if (!sameBucket_())
        out.append(s3::Feature::CrossBucketCopy);
    if (m_large)
        out.append(s3::Feature::MultipartUpload);
    return out;
}

bool CopyJob::isDestructive() const
{
    return m_mode == Mode::Move || (conflictPolicy() == ConflictPolicy::Overwrite && m_conflicts > 0);
}

QString CopyJob::targetFor_(const QString& key) const
{
    return m_targetPrefix + key.mid(m_sourceBase.size());
}

// --- plan --------------------------------------------------------------------

void CopyJob::plan()
{
    const auto failLater = [this](const QString& why) {
        QMetaObject::invokeMethod(this, [this, why] { fail_(why); }, Qt::QueuedConnection);
    };
    if (m_items.isEmpty()) {
        failLater(tr("Nothing to copy."));
        return;
    }
    for (const Item& item : m_items) {
        if (!item.key.startsWith(m_sourceBase) || item.key == m_sourceBase) {
            failLater(tr("“%1” is not below “%2”.").arg(item.key, m_sourceBase));
            return;
        }
        if (sameBucket_() && item.folder && m_targetPrefix.startsWith(item.key)) {
            failLater(tr("“%1” can't be copied into itself.").arg(item.key));
            return;
        }
    }
    m_duplicate = sameBucket_() && m_targetPrefix == m_sourceBase;
    if (m_duplicate && m_mode == Mode::Move) {
        failLater(tr("The selection is already there."));
        return;
    }
    if (m_duplicate)
        setConflictPolicy(ConflictPolicy::KeepBoth);
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    // Versioning of both buckets for the preview's words.
    ensureFresh_(buckets());
}

void CopyJob::ensureFresh_(QStringList pending)
{
    if (pending.isEmpty()) {
        nextItem_();
        return;
    }
    const QString b = pending.takeFirst();
    controller_()->capabilities()->ensureFresh(account().id, b, 600, m_run.get(),
                                               [this, rest = std::move(pending)](bool) { ensureFresh_(rest); });
}

void CopyJob::nextItem_()
{
    if (phase() != Phase::Planning)
        return;
    if (m_itemIndex >= m_items.size()) {
        planDone_();
        return;
    }
    m_current = m_items[m_itemIndex++];
    if (!m_current.folder) {
        planObject_(m_current);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    s3::ListObjectsParams source;
    source.bucket = bucket();
    source.prefix = m_current.key;   // flat: everything below, the marker included
    source.maxKeys = 1000;
    m_sourcePager = std::make_unique<s3::ListObjectsPager>(*session, source);
    s3::ListObjectsParams target;
    target.bucket = m_targetBucket;
    target.prefix = targetFor_(m_current.key);
    target.maxKeys = 1000;
    m_targetPager = std::make_unique<s3::ListObjectsPager>(*session, target);
    m_targetPage.clear();
    m_targetIndex = 0;
    m_targetDone = false;
    fetchSourcePage_();
}

void CopyJob::planObject_(const Item& item)
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    session->headObject(bucket(), item.key, QString(), m_run.get(), [this, item](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Planning)
            return;
        if (!head) {
            fail_(tr("Could not look up “%1”: %2").arg(item.key, ConnectionTester::describe(head.error())));
            return;
        }
        Record r;
        r.key = item.key;
        r.size = head->size;
        r.etag = head->etag;
        r.storageClass = head->storageClass;
        r.mtimeMs = head->lastModified.toMSecsSinceEpoch();
        r.target = targetFor_(item.key);
        s3::Session* now = session_();
        if (!now) {
            fail_(tr("Not connected."));
            return;
        }
        now->headObject(m_targetBucket, r.target, QString(), m_run.get(), [this, r](const s3::Result<s3::ObjectMeta>& there) mutable {
            if (phase() != Phase::Planning)
                return;
            if (there) {
                r.exists = true;
                r.targetSize = there->size;
                r.targetMtimeMs = there->lastModified.toMSecsSinceEpoch();
            } else if (there.error().category != s3::ErrorCategory::NotFound) {
                fail_(tr("Could not look up “%1”: %2").arg(r.target, ConnectionTester::describe(there.error())));
                return;
            }
            output_(r);
            nextItem_();
        });
    });
}

void CopyJob::fetchSourcePage_()
{
    m_sourcePager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
        if (phase() != Phase::Planning)
            return;
        if (!page) {
            fail_(tr("Could not list “%1”: %2").arg(m_current.key, ConnectionTester::describe(page.error())));
            return;
        }
        m_sourcePage = page->objects;
        m_sourceIndex = 0;
        mergeFolder_();
    });
}

// The sorted sources against the target prefix's listing: prefix
// substitution keeps the byte order, so both advance together.
void CopyJob::mergeFolder_()
{
    if (phase() != Phase::Planning)
        return;
    while (m_sourceIndex < m_sourcePage.size()) {
        const s3::ObjectEntry& e = m_sourcePage[m_sourceIndex];
        const QString target = targetFor_(e.key);
        const QByteArray want = target.toUtf8();
        while (m_targetIndex < m_targetPage.size() && m_targetPage[m_targetIndex].key.toUtf8() < want)
            ++m_targetIndex;
        if (m_targetIndex >= m_targetPage.size() && !m_targetDone) {
            m_targetPager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
                if (phase() != Phase::Planning)
                    return;
                if (!page) {
                    fail_(tr("Could not list “%1”: %2").arg(targetFor_(m_current.key), ConnectionTester::describe(page.error())));
                    return;
                }
                m_targetPage = page->objects;
                m_targetIndex = 0;
                m_targetDone = !m_targetPager->hasMore();
                mergeFolder_();
            });
            return;
        }
        Record r;
        r.key = e.key;
        r.size = e.size;
        r.etag = e.etag;
        r.storageClass = e.storageClass;
        r.mtimeMs = e.lastModified.toMSecsSinceEpoch();
        r.target = target;
        if (m_targetIndex < m_targetPage.size() && m_targetPage[m_targetIndex].key == target) {
            r.exists = true;
            r.targetSize = m_targetPage[m_targetIndex].size;
            r.targetMtimeMs = m_targetPage[m_targetIndex].lastModified.toMSecsSinceEpoch();
        }
        ++m_sourceIndex;
        output_(r);
    }
    if (m_sourcePager->hasMore()) {
        fetchSourcePage_();
        return;
    }
    m_sourcePager.reset();
    m_targetPager.reset();
    nextItem_();
}

void CopyJob::output_(const Record& in)
{
    Record r = in;
    // A folder marker is empty: one that exists already is no conflict.
    if (isMarker(r.key))
        r.exists = false;
    m_large = m_large || r.size > ObjectCopyRunner::kCopyLimit;
    m_bytes += r.size;
    if (r.exists) {
        ++m_conflicts;
        if (m_conflictSamples.size() < kConflictSamples) {
            m_conflictSamples.append(tr("%1 — here %2; there %3")
                                         .arg(r.target, describeSide(r.size, r.mtimeMs),
                                              describeSide(r.targetSize, r.targetMtimeMs)));
        }
    }
    if (m_samples.size() < kSamples)
        m_samples.append(r.key);
    m_plan->appendRecords({r.fields()});
    setProgress_(m_plan->count(), 0);
}

void CopyJob::planDone_()
{
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        if (m_plan->count() == 0) {
            m_verification = tr("nothing to copy");
            finish_(report_());
            return;
        }
        const bool move = m_mode == Mode::Move;
        Preview p;
        p.action = move ? tr("Move") : tr("Copy");
        p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_items.size() == 1 ? m_items.first().key : m_sourceBase);
        p.objects = m_plan->count();
        p.bytes = m_bytes;
        p.samples = m_samples;
        p.details.append(tr("To: s3://%1/%2").arg(m_targetBucket, m_targetPrefix));
        p.details.append(move ? tr("Each object is copied by the server, the copy checked, and only then the "
                                   "original deleted.")
                              : tr("Each object is copied by the server and the copy checked."));
        if (m_duplicate)
            p.details.append(tr("Copies into the same folder are made under a new name."));
        p.conflicts = m_conflicts;
        p.conflictSamples = m_conflictSamples;
        p.keepBothLabel = tr("Keep both (copy as “name (1)”)");
        if (m_duplicate)
            p.policies = {ConflictPolicy::KeepBoth, ConflictPolicy::Skip};
        if (m_conflicts > 0 && !m_duplicate) {
            if (keepsVersions(controller_(), account().id, m_targetBucket))
                p.details.append(tr("The target bucket keeps versions: overwritten objects stay as earlier versions."));
            else
                p.warnings.append(tr("Overwriting replaces those objects for good: the target bucket keeps no "
                                     "versions."));
        }
        if (move) {
            if (keepsVersions(controller_(), account().id, bucket())) {
                p.details.append(tr("The bucket keeps versions: the originals stay as earlier versions, hidden behind "
                                    "delete markers."));
            } else {
                p.permanent = true;   // the dialog says it: nothing to recover
            }
        }
        p.warnings.append(tr("Object ACLs are not copied: the copies get the bucket's default access."));
        setProgress_(0, p.objects);
        setBytes_(0, m_bytes);
        setPreview_(p);
    });
}

// --- execute -----------------------------------------------------------------

void CopyJob::execute()
{
    m_plan->rewind();
    m_eof = false;
    setProgress_(0, m_plan->count());
    pump_();
}

void CopyJob::pump_()
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
        ++m_inFlight;
        startRecord_(Record::from(records.first()));
        pump_();
    });
}

void CopyJob::startRecord_(const Record& r)
{
    // Overwriting needs no look; a marker is never a conflict.
    if (conflictPolicy() == ConflictPolicy::Overwrite || isMarker(r.key)) {
        copy_(r, r.target);
        return;
    }
    // Right before writing: what is there now (not what the plan saw).
    s3::Session* session = session_();
    if (!session) {
        --m_inFlight;
        addFailure_(r.key, tr("not copied (not connected) — the original is untouched"));
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
        return;
    }
    session->headObject(m_targetBucket, r.target, QString(), m_run.get(), [this, r](const s3::Result<s3::ObjectMeta>& head) {
        if (head) {
            if (conflictPolicy() == ConflictPolicy::KeepBoth) {
                findFreeName_(r, 1);
                return;
            }
            skipped_(r, tr("exists at the target"));
            return;
        }
        if (head.error().category != s3::ErrorCategory::NotFound) {
            --m_inFlight;
            addFailure_(r.key, tr("could not look up the target (%1) — the original is untouched")
                                   .arg(ConnectionTester::describe(head.error())));
            setProgress_(progressDone() + 1, progressTotal());
            pump_();
            return;
        }
        copy_(r, r.target);
    });
}

// "name (n).ext" that doesn't exist yet.
void CopyJob::findFreeName_(const Record& r, int n)
{
    static constexpr int kMaxTries = 100;
    s3::Session* session = session_();
    if (!session || n > kMaxTries) {
        --m_inFlight;
        addFailure_(r.key, session ? tr("no free name for a second copy") : tr("not copied (not connected)"));
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
        return;
    }
    const QString candidate = UploadJob::keepBothName(r.target, n);
    session->headObject(m_targetBucket, candidate, QString(), m_run.get(),
                        [this, r, n, candidate](const s3::Result<s3::ObjectMeta>& head) {
        if (head) {
            findFreeName_(r, n + 1);
            return;
        }
        if (head.error().category != s3::ErrorCategory::NotFound) {
            --m_inFlight;
            addFailure_(r.key, tr("could not look up the target (%1) — the original is untouched")
                                   .arg(ConnectionTester::describe(head.error())));
            setProgress_(progressDone() + 1, progressTotal());
            pump_();
            return;
        }
        copy_(r, candidate);
    });
}

void CopyJob::copy_(const Record& r, const QString& target)
{
    ObjectCopyRunner::Item item;
    item.sourceBucket = bucket();
    item.sourceKey = r.key;
    item.size = r.size;
    item.etag = r.etag;
    item.storageClass = r.storageClass;
    item.bucket = m_targetBucket;
    item.key = target;
    item.deleteSource = m_mode == Mode::Move;
    m_runner->run(item, [this](const ObjectCopyRunner::Item& done, const ObjectCopyRunner::Result& result) {
        --m_inFlight;
        if (result.ok) {
            ++m_succeeded;
            setBytes_(bytesDone() + done.size, bytesTotal());
        } else if (!result.cancelled) {
            addFailure_(done.sourceKey, result.failure);
        }
        setProgress_(progressDone() + 1, progressTotal());
        pump_();
    });
}

void CopyJob::skipped_(const Record& r, const QString& why)
{
    --m_inFlight;
    ++m_skipped;
    if (m_notes.size() < kMaxListed)
        m_notes.append(QStringLiteral("%1: %2").arg(r.key, why));
    setProgress_(progressDone() + 1, progressTotal());
    pump_();
}

void CopyJob::settled_()
{
    if (phase() != Phase::Executing || m_inFlight > 0 || m_reading)
        return;
    if (isCancelled()) {
        m_verification = tr("cancelled — %1 of %2 done").arg(m_succeeded).arg(m_plan->count());
        finish_(report_());
        return;
    }
    if (m_eof)
        executed_();
}

void CopyJob::onCancel()
{
    switch (phase()) {
    case Phase::Planning:
        m_run.reset();
        m_sourcePager.reset();
        m_targetPager.reset();
        break;
    case Phase::Executing:
        m_runner->cancel();   // multipart copies abort their uploads; sources stay
        settled_();
        break;
    case Phase::Verifying:
        m_run.reset();
        m_verification = tr("done; cancelled while verifying");
        finish_(report_());
        break;
    default:
        break;
    }
}

// --- verify ------------------------------------------------------------------

void CopyJob::verify()
{
    if (m_mode == Mode::Copy) {
        // Every copy was looked up and compared on its own.
        m_verified = m_failedKeys.isEmpty();
        m_verification = m_succeeded > 0 ? tr("every copy checked (size, and the checksum where the ETag is one)")
                                         : QString();
        finish_(report_());
        return;
    }
    m_leftovers = 0;
    verifyItem_(0);
}

// A move: the sources hold nothing but what failed (a folder's first page,
// the first kVerifyItems items).
void CopyJob::verifyItem_(int index)
{
    if (index >= m_items.size() || index >= kVerifyItems) {
        if (m_leftovers > 0 && !m_rechecked) {
            // Listings may lag behind deletes for a moment (Hetzner).
            m_rechecked = true;
            m_leftovers = 0;
            QTimer::singleShot(m_recheckMs, m_run.get(), [this] { verifyItem_(0); });
            return;
        }
        m_verified = m_leftovers == 0;
        m_verification = m_verified ? tr("verified: the originals are gone")
                                    : tr("not verified: %n original(s) still there", nullptr, int(m_leftovers));
        finish_(report_());
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        m_verification = tr("not verified (not connected)");
        finish_(report_());
        return;
    }
    const Item item = m_items[index];
    if (!item.folder) {
        session->headObject(bucket(), item.key, QString(), m_run.get(), [this, item, index](const s3::Result<s3::ObjectMeta>& head) {
            if (head && !m_failedSet.contains(item.key))
                ++m_leftovers;
            verifyItem_(index + 1);
        });
        return;
    }
    s3::ListObjectsParams params;
    params.bucket = bucket();
    params.prefix = item.key;
    params.maxKeys = 1000;
    session->listObjects(params, m_run.get(), [this, index](const s3::Result<s3::ListPage>& page) {
        if (!page) {
            m_verification = tr("not verified: %1").arg(ConnectionTester::describe(page.error()));
            finish_(report_());
            return;
        }
        for (const s3::ObjectEntry& e : page->objects)
            m_leftovers += m_failedSet.contains(e.key) ? 0 : 1;
        verifyItem_(index + 1);
    });
}

void CopyJob::addFailure_(const QString& key, const QString& reason)
{
    if (m_failedSet.contains(key))
        return;
    m_failedSet.insert(key);
    m_failedKeys.append(key);
    if (m_failures.size() < kMaxListed)
        m_failures.append(QStringLiteral("%1: %2").arg(key, reason));
}

Job::Report CopyJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped;
    r.failed = m_failedKeys.size();
    r.failures = m_failures;
    r.notes = m_notes;
    r.verification = m_verification;
    r.verified = m_verified;
    return r;
}

} // namespace qbucket
