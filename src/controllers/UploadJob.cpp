#include "controllers/UploadJob.h"

#include "controllers/AccountSessions.h"
#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"

#include <qbucket/s3/transfer/Uploader.h>

#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
#include <QThreadPool>
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

QString contentTypeFor(const QString& path)
{
    static const QMimeDatabase db;
    const QMimeType t = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    return t.isDefault() ? QString() : t.name();
}

} // namespace

// --- records --------------------------------------------------------------------

QStringList UploadJob::Record::fields() const
{
    return {key, path, QString::number(size), QString::number(mtimeMs), marker ? QStringLiteral("d") : QStringLiteral("f"),
            exists ? QStringLiteral("1") : QString(), QString::number(remoteSize), QString::number(remoteMtimeMs),
            remoteEtag};
}

UploadJob::Record UploadJob::Record::from(const QStringList& f)
{
    Record r;
    r.key = f.value(0);
    r.path = f.value(1);
    r.size = f.value(2).toLongLong();
    r.mtimeMs = f.value(3).toLongLong();
    r.marker = f.value(4) == QLatin1String("d");
    r.exists = f.value(5) == QLatin1String("1");
    r.remoteSize = f.size() > 6 ? f.value(6).toLongLong() : -1;
    r.remoteMtimeMs = f.value(7).toLongLong();
    r.remoteEtag = f.value(8);
    return r;
}

// --- construction ---------------------------------------------------------------

UploadJob::UploadJob(const Account& account, const QString& bucket, const QString& prefix, const QStringList& sources,
                     const QString& planDir, Options options, QObject* parent)
    : Job(account, bucket, parent),
      m_prefix(prefix),
      m_sources(sources),
      m_options(options),
      m_planDir(planDir)
{
}

UploadJob::UploadJob(const Account& account, const QString& bucket, const QList<Item>& items, const QString& planDir,
                     QObject* parent)
    : Job(account, bucket, parent),
      m_explicit(items),
      m_explicitMode(true),
      m_planDir(planDir)
{
}

UploadJob::UploadJob(const Account& account, const TransferJobStore::Record& record, QObject* parent)
    : Job(account, record.bucket, parent),
      m_prefix(record.prefix),
      m_resuming(true),
      m_record(record)
{
    setConflictPolicy(record.policy);
}

UploadJob::~UploadJob()
{
    if (m_scanCancel)
        m_scanCancel->store(true);
    m_active.clear();   // uploaders give their slots back
}

QString UploadJob::title() const
{
    if (m_resuming)
        return m_record.title;
    if (m_explicitMode)
        return tr("Upload %n file(s) to s3://%1", nullptr, int(m_explicit.size())).arg(bucket());
    if (m_sources.size() == 1)
        return tr("Upload “%1” to s3://%2/%3").arg(QFileInfo(m_sources.first()).fileName(), bucket(), m_prefix);
    return tr("Upload %n item(s) to s3://%1/%2", nullptr, int(m_sources.size())).arg(bucket(), m_prefix);
}

bool UploadJob::isDestructive() const
{
    // A resumed job doesn't know its conflicts any more: assume some.
    return conflictPolicy() == ConflictPolicy::Overwrite && (m_conflicts > 0 || m_resuming);
}

bool UploadJob::needsConfirmation() const
{
    if (m_resuming)
        return false;   // confirmed in the resume dialog
    return m_options.alwaysConfirm || m_explicitMode || m_conflicts > 0 || m_files + m_markers != 1
        || m_scan.skippedCount > 0 || account().environment == Environment::Production;
}

QList<Job::ActiveItem> UploadJob::activeItems() const
{
    QList<ActiveItem> out;
    for (const auto& a : m_active) {
        if (a->uploader)
            out.append({a->key, a->uploader->bytesDone(), a->record.size});
        else
            out.append({a->key, 0, a->record.size});
    }
    return out;
}

QString UploadJob::keepBothName(const QString& key, int n)
{
    const qsizetype slash = key.lastIndexOf(u'/');
    const QString dir = key.left(slash + 1);
    const QString name = key.mid(slash + 1);
    // The extension starts at the first dot that isn't the name's first character.
    const qsizetype dot = name.indexOf(u'.', 1);
    const QString base = dot < 0 ? name : name.left(dot);
    const QString ext = dot < 0 ? QString() : name.mid(dot);
    return QStringLiteral("%1%2 (%3)%4").arg(dir, base).arg(n).arg(ext);
}

// --- plan -----------------------------------------------------------------------

TransferJobStore* UploadJob::jobStore_() const
{
    TransferService* t = controller_() ? controller_()->transfers() : nullptr;
    return t ? t->jobs() : nullptr;
}

QString UploadJob::transferId_(const Record& r) const
{
    // Stable across sessions: the job's first id and the item's place in the plan.
    return QStringLiteral("%1-%2").arg(m_record.id.isEmpty() ? id() : m_record.id).arg(r.ordinal);
}

void UploadJob::plan()
{
    m_run = std::make_unique<QObject>();
    if (m_resuming) {
        resumePlan_();
        return;
    }
    const QString rawPath = m_planDir + u'/' + id() + QStringLiteral(".scan");
    if (m_explicitMode) {
        // Retry: exactly these files; each target looked up by HEAD.
        m_raw = std::make_unique<PlanFile>(rawPath);
        QList<QStringList> records;
        for (const Item& i : std::as_const(m_explicit)) {
            const QFileInfo info(i.localPath);
            if (!info.isFile()) {
                ++m_scan.skippedCount;
                m_scan.skipped.append(tr("%1: not found").arg(QDir::toNativeSeparators(i.localPath)));
                continue;
            }
            records.append({i.key, i.localPath, QString::number(info.size()),
                            QString::number(info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch()), QStringLiteral("f"),
                            QStringLiteral("0")});
            ++m_scan.files;
            m_scan.bytes += info.size();
        }
        m_raw->appendRecords(records);
        m_raw->flush([this](bool ok) {
            if (phase() != Phase::Planning)
                return;
            if (!ok) {
                fail_(tr("The plan could not be written: %1").arg(m_raw->errorText()));
                return;
            }
            TransferJobStore* jobs = jobStore_();
            m_plan = std::make_unique<PlanFile>(jobs ? jobs->planPath(id()) : m_planDir + u'/' + id() + QStringLiteral(".plan"));
            mergeNext_();
        });
        return;
    }

    // Walk on a worker; the GUI only shows how many files were found.
    m_scanCancel = std::make_shared<std::atomic<bool>>(false);
    m_scanFound = std::make_shared<std::atomic<qint64>>(0);
    m_scanProgress = new QTimer(m_run.get());
    m_scanProgress->setInterval(200);
    connect(m_scanProgress, &QTimer::timeout, this, [this] { setProgress_(m_scanFound->load(), 0); });
    m_scanProgress->start();
    const QPointer<UploadJob> self(this);
    auto* relay = new QObject;   // carries the result back; deletes itself
    relay->moveToThread(thread());
    QThreadPool::globalInstance()->start([relay, self, sources = m_sources, prefix = m_prefix, rawPath,
                                          cancel = m_scanCancel, found = m_scanFound] {
        LocalScanner::Result r = LocalScanner::scan(sources, prefix, rawPath, *cancel, *found);
        QMetaObject::invokeMethod(relay, [relay, self, r] {
            if (self)
                self->scanned_(r);
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

void UploadJob::scanned_(const LocalScanner::Result& result)
{
    if (m_scanProgress)
        m_scanProgress->stop();
    if (phase() != Phase::Planning)
        return;
    m_scan = result;
    if (!result.error.isEmpty()) {
        fail_(tr("The plan could not be written: %1").arg(result.error));
        return;
    }
    setProgress_(result.files + result.folders, 0);
    m_raw = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".scan"), PlanFile::Mode::Existing,
                                       result.files + result.folders);
    TransferJobStore* jobs = jobStore_();
    m_plan = std::make_unique<PlanFile>(jobs ? jobs->planPath(id()) : m_planDir + u'/' + id() + QStringLiteral(".plan"));
    mergeNext_();
}

// An earlier session's job: its plan is on disk, with what is done already
// and which files have part records.
void UploadJob::resumePlan_()
{
    TransferJobStore* jobs = jobStore_();
    TransferService* transfers = controller_()->transfers();
    if (!jobs || !transfers) {
        fail_(tr("Nothing to resume."));
        return;
    }
    if (!QFile::exists(jobs->planPath(m_record.id))) {
        fail_(tr("The plan of this upload is gone; it can't be continued."));
        return;
    }
    jobs->loadDone(m_record.id, m_run.get(), [this, transfers](const QSet<qint64>& done) {
        if (phase() != Phase::Planning)
            return;
        m_doneOrdinals = done;
        auto store = transfers->resumeStore();
        auto* relay = new QObject;
        relay->moveToThread(thread());
        const QPointer<UploadJob> self(this);
        QThreadPool::globalInstance()->start([relay, self, store] {
            const QStringList ids = store->ids();
            QMetaObject::invokeMethod(relay, [relay, self, ids] {
                if (self && self->phase() == Phase::Planning) {
                    self->m_storedTransfers = QSet<QString>(ids.begin(), ids.end());
                    Preview p;
                    p.action = tr("Resume");
                    p.scope = QStringLiteral("s3://%1/%2").arg(self->bucket(), self->m_prefix);
                    p.objects = std::max<qint64>(0, self->m_record.items - self->m_doneOrdinals.size());
                    p.bytes = -1;
                    self->m_files = self->m_record.items;
                    self->m_bytes = self->m_record.bytes;
                    self->m_processed = self->m_doneOrdinals.size();
                    self->setPreview_(p);
                }
                relay->deleteLater();
            }, Qt::QueuedConnection);
        });
    });
}

QString UploadJob::groupOf_(const QString& key) const
{
    // A record below one of the uploaded folders: that folder's name.
    if (m_explicitMode || !key.startsWith(m_prefix))
        return {};
    const qsizetype slash = key.indexOf(u'/', m_prefix.size());
    return slash < 0 ? QString() : key.mid(m_prefix.size(), slash - m_prefix.size());
}

// Merges the sorted local records with the targets: a listing of each
// uploaded folder's prefix (page by page, as far as needed), HEAD for
// single files. Resumes itself after every asynchronous step.
void UploadJob::mergeNext_()
{
    if (phase() != Phase::Planning || m_fetching)
        return;
    while (m_cursor < m_batch.size()) {
        const Record r = Record::from(m_batch[m_cursor]);
        const QString group = groupOf_(r.key);
        if (group.isEmpty()) {
            if (m_headsInFlight >= kParallelHeads)
                return;   // a HEAD's answer continues
            ++m_cursor;
            mergeHead_(r);
            continue;
        }
        if (group != m_group || !m_pager) {
            s3::Session* session = session_();
            if (!session) {
                fail_(tr("Not connected."));
                return;
            }
            m_group = group;
            s3::ListObjectsParams params;
            params.bucket = bucket();
            params.prefix = m_prefix + group + u'/';
            params.maxKeys = 1000;
            m_pager = std::make_unique<s3::ListObjectsPager>(*session, params);
            m_remote.clear();
            m_remoteIndex = 0;
            m_groupDone = false;
        }
        const QByteArray key = r.key.toUtf8();
        while (m_remoteIndex < m_remote.size() && m_remote[m_remoteIndex].key.toUtf8() < key)
            ++m_remoteIndex;
        if (m_remoteIndex >= m_remote.size() && !m_groupDone) {
            // Everything listed so far sorts before this key: the next page.
            m_fetching = true;
            m_pager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
                m_fetching = false;
                if (phase() != Phase::Planning)
                    return;
                if (!page) {
                    fail_(tr("Could not list “%1”: %2").arg(m_prefix + m_group, ConnectionTester::describe(page.error())));
                    return;
                }
                m_remote = page->objects;
                m_remoteIndex = 0;
                m_groupDone = !m_pager->hasMore();
                mergeNext_();
            });
            return;
        }
        Record out = r;
        if (m_remoteIndex < m_remote.size() && m_remote[m_remoteIndex].key.toUtf8() == key) {
            const s3::ObjectEntry& e = m_remote[m_remoteIndex];
            out.exists = true;
            out.remoteSize = e.size;
            out.remoteMtimeMs = e.lastModified.toMSecsSinceEpoch();
            out.remoteEtag = e.etag;
        }
        ++m_cursor;
        output_(out);
    }
    if (m_headsInFlight > 0)
        return;
    if (m_batchesDone) {
        planDone_();
        return;
    }
    // The next batch of local records.
    m_fetching = true;
    m_raw->readNextRecords(500, [this](const QList<QStringList>& records) {
        m_fetching = false;
        if (phase() != Phase::Planning)
            return;
        m_batch = records;
        m_cursor = 0;
        m_batchesDone = records.isEmpty();
        mergeNext_();
    });
}

void UploadJob::mergeHead_(const Record& r)
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    ++m_headsInFlight;
    session->headObject(bucket(), r.key, QString(), m_run.get(), [this, r](const s3::Result<s3::ObjectMeta>& head) {
        --m_headsInFlight;
        if (phase() != Phase::Planning)
            return;
        Record out = r;
        if (head) {
            out.exists = true;
            out.remoteSize = head->size;
            out.remoteMtimeMs = head->lastModified.toMSecsSinceEpoch();
            out.remoteEtag = head->etag;
        } else if (head.error().category != s3::ErrorCategory::NotFound) {
            fail_(tr("Could not look up “%1”: %2").arg(r.key, ConnectionTester::describe(head.error())));
            return;
        }
        output_(out);
        mergeNext_();
    });
}

void UploadJob::output_(const Record& r)
{
    if (r.marker) {
        if (r.exists)
            return;   // the folder is there already: nothing to do
        ++m_markers;
    } else {
        ++m_files;
        m_bytes += r.size;
        if (r.exists) {
            ++m_conflicts;
            if (m_conflictSamples.size() < kConflictSamples) {
                m_conflictSamples.append(tr("%1 — here %2; there %3")
                                             .arg(r.key, describeSide(r.size, r.mtimeMs),
                                                  describeSide(r.remoteSize, r.remoteMtimeMs)));
            }
        }
    }
    if (m_samples.size() < kSamples)
        m_samples.append(r.key);
    m_plan->appendRecords({r.fields()});
    setProgress_(m_files + m_markers, m_scan.files + m_scan.folders);
}

void UploadJob::planDone_()
{
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        m_raw.reset();
        m_notes = m_scan.skipped;
        if (m_files + m_markers == 0) {
            finish_(report_());
            return;
        }
        Preview p;
        p.action = tr("Upload");
        p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_explicitMode ? QString() : m_prefix);
        p.objects = m_files + m_markers;
        p.bytes = m_bytes;
        p.samples = m_samples;
        if (m_markers > 0)
            p.details.append(tr("%n empty folder(s) become folder markers.", nullptr, int(m_markers)));
        if (m_scan.skippedCount > 0) {
            p.warnings.append(tr("%n file(s) or folder(s) are left out (links to folders, unreadable or "
                                 "special files, names too long) — first: %1",
                                 nullptr, int(m_scan.skippedCount))
                                  .arg(m_scan.skipped.value(0)));
        }
        p.conflicts = m_conflicts;
        p.conflictSamples = m_conflictSamples;
        if (m_conflicts > 0) {
            const std::optional<s3::CapabilitySet> state =
                controller_()->capabilities()->bucketSet(account().id, bucket());
            if (state && state->bucketState.versioning == QLatin1String("Enabled"))
                p.details.append(tr("The bucket keeps versions: overwritten objects stay as earlier versions."));
            else
                p.warnings.append(tr("Overwriting replaces those objects for good: the bucket keeps no versions."));
        }
        setProgress_(0, p.objects);
        setBytes_(0, m_bytes);
        setPreview_(p);
    });
}

// --- execute --------------------------------------------------------------------

void UploadJob::execute()
{
    // A key that must not be overwritten: If-None-Match only where the
    // server is known to honour it (Hetzner and Garage ignore it).
    m_conditional = controller_()->capabilities()->decide(account().id, bucket(), s3::Feature::ConditionalWrites,
                                                          false) == s3::Decision::Allowed;
    TransferJobStore* jobs = jobStore_();
    if (m_resuming) {
        m_plan = std::make_unique<PlanFile>(jobs->planPath(m_record.id), PlanFile::Mode::Existing, m_record.items);
        m_plan->setKeepFile(true);
    } else if (jobs) {
        // From now on the job can continue in a later session.
        m_plan->setKeepFile(true);
        m_record.id = id();
        m_record.kind = TransferKind::Upload;
        m_record.account = account().id;
        m_record.bucket = bucket();
        m_record.title = title();
        m_record.prefix = m_prefix;
        m_record.policy = conflictPolicy();
        m_record.items = m_files + m_markers;
        m_record.bytes = m_bytes;
        m_record.started = QDateTime::currentDateTimeUtc();
        jobs->save(m_record);
    }
    m_plan->rewind();
    m_progressClock.start();
    setProgress_(0, m_files + m_markers);
    setBytes_(0, m_bytes);
    pump_();
}

void UploadJob::pump_()
{
    if (phase() != Phase::Executing)
        return;
    if (isCancelled() || isPaused()) {
        maybeExecuted_();
        return;
    }
    while (m_active.size() + m_inFlightMisc < kParallelFiles && !m_queue.empty()) {
        const Record r = m_queue.front();
        m_queue.pop_front();
        startItem_(r);
    }
    if (m_queue.size() < kParallelFiles && !m_readingPlan && !m_planExhausted) {
        m_readingPlan = true;
        m_plan->readNextRecords(200, [this](const QList<QStringList>& records) {
            m_readingPlan = false;
            if (phase() != Phase::Executing)
                return;
            if (records.isEmpty())
                m_planExhausted = true;
            for (const QStringList& f : records) {
                Record r = Record::from(f);
                r.ordinal = m_readOrdinal++;
                if (m_doneOrdinals.contains(r.ordinal)) {
                    m_doneBytes += r.size;   // finished in an earlier session
                    continue;
                }
                m_queue.push_back(r);
            }
            pump_();
        });
    }
    maybeExecuted_();
}

void UploadJob::markDone_(const Record& r)
{
    if (TransferJobStore* jobs = jobStore_(); jobs && r.ordinal >= 0 && !m_record.id.isEmpty())
        jobs->markDone(m_record.id, r.ordinal);
}

void UploadJob::end_()
{
    if (TransferJobStore* jobs = jobStore_(); jobs && !m_record.id.isEmpty()) {
        if (isSuspended()) {
            m_record.suspended = true;
            jobs->save(m_record);
        } else {
            jobs->remove(m_record.id);
        }
    }
    finish_(report_());
}

void UploadJob::suspend()
{
    if (phase() != Phase::Executing) {
        cancel();
        return;
    }
    markSuspended_();
    cancel();   // onCancel keeps every part record
}

void UploadJob::maybeExecuted_()
{
    if (phase() != Phase::Executing || !m_active.isEmpty() || m_inFlightMisc > 0 || m_readingPlan)
        return;
    if (isCancelled()) {
        updateProgress_(true);
        end_();
        return;
    }
    if (isPaused())
        return;
    if (m_planExhausted && m_queue.empty()) {
        updateProgress_(true);
        executed_();
    }
}

void UploadJob::startItem_(const Record& r)
{
    if (r.marker) {
        putMarker_(r);
        return;
    }
    if (m_resuming && m_storedTransfers.contains(transferId_(r))) {
        // Under way when the last session ended: continue its parts.
        s3::Session* session = session_();
        TransferService* transfers = controller_()->transfers();
        if (session && transfers) {
            auto item = std::make_shared<Active>();
            item->record = r;
            item->key = r.key;
            item->uploader = s3::transfer::Uploader::fromStore(session, transfers->resumeStore(), transferId_(r),
                                                               transfers->limiter(account().id));
            m_active.append(item);
            Active* raw = item.get();
            connect(item->uploader.get(), &s3::transfer::Uploader::finished, this, [this, raw] { itemDone_(raw); });
            connect(item->uploader.get(), &s3::transfer::Uploader::progress, this, [this] { updateProgress_(false); });
            item->uploader->start();
            if (isPaused())
                item->uploader->pause();
            return;
        }
    }
    if (r.exists) {
        switch (conflictPolicy()) {
        case ConflictPolicy::Skip:
            itemSkipped_(r, tr("exists already"));
            return;
        case ConflictPolicy::Overwrite:
            break;
        case ConflictPolicy::KeepBoth: {
            auto item = std::make_shared<Active>();
            item->record = r;
            item->keepBothTry = 1;
            item->key = keepBothName(r.key, 1);
            m_active.append(item);
            resolveTarget_(item);
            return;
        }
        }
    }
    auto item = std::make_shared<Active>();
    item->record = r;
    item->key = r.key;
    m_active.append(item);
    if (r.exists) {   // overwrite, as confirmed
        upload_(item);
        return;
    }
    resolveTarget_(item);
}

// Before writing a key that must not be overwritten: If-None-Match where
// it is honoured, otherwise HEAD right before the upload.
void UploadJob::resolveTarget_(const std::shared_ptr<Active>& item)
{
    if (m_conditional) {
        item->conditional = true;
        upload_(item);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        m_active.removeOne(item);
        itemFailed_(item->record, item->key, tr("Not connected."));
        pump_();
        return;
    }
    session->headObject(bucket(), item->key, QString(), m_run.get(), [this, item](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Executing || !m_active.contains(item))
            return;
        if (!head && head.error().category == s3::ErrorCategory::NotFound) {
            upload_(item);
            return;
        }
        if (!head) {
            m_active.removeOne(item);
            itemFailed_(item->record, item->key, ConnectionTester::describe(head.error()));
            pump_();
            return;
        }
        // Taken (meanwhile, or the "keep both" name).
        if (conflictPolicy() == ConflictPolicy::KeepBoth && item->keepBothTry < 100) {
            item->key = keepBothName(item->record.key, ++item->keepBothTry);
            resolveTarget_(item);
            return;
        }
        m_active.removeOne(item);
        if (conflictPolicy() == ConflictPolicy::Overwrite)
            itemFailed_(item->record, item->key, tr("appeared on the server after the plan; not overwritten unconfirmed"));
        else
            itemSkipped_(item->record, tr("appeared on the server meanwhile"));
        pump_();
    });
}

void UploadJob::upload_(const std::shared_ptr<Active>& item)
{
    s3::Session* session = session_();
    TransferService* transfers = controller_()->transfers();
    if (!session || !transfers) {
        m_active.removeOne(item);
        itemFailed_(item->record, item->key, tr("Not connected."));
        pump_();
        return;
    }
    s3::transfer::UploadRequest req;
    req.bucket = bucket();
    req.key = item->key;
    req.localPath = item->record.path;
    req.contentType = contentTypeFor(item->record.path);
    req.ifNoneMatch = item->conditional;
    req.transferId = transferId_(item->record);
    req.account = account().id.toString(QUuid::WithoutBraces);
    req.extra.insert(QStringLiteral("job"), m_record.id.isEmpty() ? id() : m_record.id);
    item->uploader = std::make_unique<s3::transfer::Uploader>(session, req, transfers->limiter(account().id),
                                                              transfers->resumeStore());
    Active* raw = item.get();
    connect(item->uploader.get(), &s3::transfer::Uploader::finished, this, [this, raw] { itemDone_(raw); });
    connect(item->uploader.get(), &s3::transfer::Uploader::progress, this, [this] { updateProgress_(false); });
    item->uploader->start();
    if (isPaused())
        item->uploader->pause();
}

void UploadJob::putMarker_(const Record& r)
{
    s3::Session* session = session_();
    if (!session) {
        itemFailed_(r, r.key, tr("Not connected."));
        return;
    }
    ++m_inFlightMisc;
    const auto put = [this, r, session] {
        s3::PutObjectParams params;
        params.bucket = bucket();
        params.key = r.key;
        params.contentType = QStringLiteral("application/x-directory");
        params.ifNoneMatch = m_conditional;
        session->putObject(params, m_run.get(), [this, r](const s3::Result<s3::PutObjectResult>& put) {
            --m_inFlightMisc;
            if (phase() != Phase::Executing)
                return;
            Journal::Record rec = record_(s3::operationName(s3::Operation::PutObject), r.key);
            if (put) {
                rec.httpStatus = 200;
                rec.requestId = put->requestId;
                rec.versionId = put->versionId;
                journal_({rec});
                ++m_succeeded;
                ++m_processed;
                markDone_(r);
            } else if (put.error().category == s3::ErrorCategory::PreconditionFailed) {
                itemSkipped_(r, tr("exists already"));
            } else {
                rec.result = Journal::Result::Failed;
                rec.httpStatus = put.error().httpStatus;
                rec.message = put.error().toString();
                journal_({rec});
                itemFailed_(r, r.key, ConnectionTester::describe(put.error()));
            }
            updateProgress_(false);
            pump_();
        });
    };
    if (m_conditional) {
        put();
        return;
    }
    session->headObject(bucket(), r.key, QString(), m_run.get(), [this, r, put](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Executing) {
            --m_inFlightMisc;
            return;
        }
        if (head) {
            --m_inFlightMisc;
            itemSkipped_(r, tr("exists already"));
            pump_();
            return;
        }
        put();
    });
}

void UploadJob::itemDone_(Active* raw)
{
    std::shared_ptr<Active> item;
    for (const auto& a : std::as_const(m_active)) {
        if (a.get() == raw)
            item = a;
    }
    if (!item || phase() != Phase::Executing)
        return;
    m_active.removeOne(item);
    const s3::Result<s3::transfer::UploadResult>& r = item->uploader->result();
    Journal::Record rec = record_(r && r->multipart ? QStringLiteral("MultipartUpload")
                                                    : s3::operationName(s3::Operation::PutObject),
                                  item->key);
    if (r) {
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        rec.versionId = r->versionId;
        rec.message = QStringLiteral("%1 %2 parts, %3").arg(r->etag).arg(r->parts)
                          .arg(r->integrity == s3::Integrity::Md5Verified ? QStringLiteral("md5 verified")
                                                                         : QStringLiteral("size verified"));
        journal_({rec});
        ++m_succeeded;
        ++m_processed;
        markDone_(item->record);
        m_doneBytes += item->record.size;
        if (r->integrity == s3::Integrity::Md5Verified)
            ++m_md5Verified;
        else
            ++m_sizeVerified;
    } else {
        const s3::Error& e = r.error();
        if (e.category == s3::ErrorCategory::Cancelled) {
            rec.result = Journal::Result::Skipped;
            rec.message = e.toString();
            journal_({rec});
            ++m_processed;
        } else if (e.category == s3::ErrorCategory::PreconditionFailed && item->conditional) {
            // If-None-Match refused it: the key appeared meanwhile.
            rec.result = Journal::Result::Skipped;
            rec.httpStatus = e.httpStatus;
            rec.message = e.toString();
            journal_({rec});
            if (conflictPolicy() == ConflictPolicy::KeepBoth && item->keepBothTry < 100) {
                auto again = std::make_shared<Active>();
                again->record = item->record;
                again->keepBothTry = item->keepBothTry + 1;
                again->key = keepBothName(item->record.key, again->keepBothTry);
                m_active.append(again);
                resolveTarget_(again);
                return;
            }
            itemSkipped_(item->record, tr("appeared on the server meanwhile"));
        } else {
            rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message = e.toString();
            journal_({rec});
            itemFailed_(item->record, item->key, ConnectionTester::describe(e));
        }
    }
    // The uploader may still be inside its own signal: delete it later.
    item->uploader.release()->deleteLater();
    updateProgress_(false);
    pump_();
}

void UploadJob::itemSkipped_(const Record& r, const QString& why)
{
    ++m_skipped;
    ++m_processed;
    markDone_(r);
    if (m_notes.size() < kMaxListed)
        m_notes.append(QStringLiteral("%1: %2").arg(r.key, why));
    updateProgress_(false);
}

void UploadJob::itemFailed_(const Record& r, const QString& key, const QString& reason)
{
    ++m_failed;
    ++m_processed;
    markDone_(r);
    if (m_failures.size() < kMaxListed)
        m_failures.append(QStringLiteral("%1: %2").arg(key, reason));
    if (!r.marker)
        m_failedItems.append({r.path, r.key});
    updateProgress_(false);
}

void UploadJob::updateProgress_(bool force)
{
    if (!force && m_progressClock.isValid() && m_progressClock.elapsed() < 100)
        return;
    m_progressClock.start();
    qint64 bytes = m_doneBytes;
    for (const auto& a : std::as_const(m_active)) {
        if (a->uploader)
            bytes += a->uploader->bytesDone();
    }
    setProgress_(m_processed, m_files + m_markers);
    setBytes_(bytes, m_bytes);
}

void UploadJob::onPause()
{
    for (const auto& a : std::as_const(m_active)) {
        if (a->uploader)
            a->uploader->pause();
    }
}

void UploadJob::onResume()
{
    for (const auto& a : std::as_const(m_active)) {
        if (a->uploader)
            a->uploader->resume();
    }
    pump_();
}

void UploadJob::onCancel()
{
    if (m_scanCancel)
        m_scanCancel->store(true);
    if (phase() == Phase::Planning) {
        m_run.reset();   // drops every pending answer
        m_pager.reset();
        return;
    }
    if (phase() != Phase::Executing)
        return;
    m_queue.clear();
    m_planExhausted = true;
    const auto active = m_active;
    for (const auto& a : active) {
        // Suspended: the part records stay for the next session; otherwise
        // the multipart upload is aborted. finished follows either way.
        if (a->uploader)
            a->uploader->cancel(isSuspended());
    }
    // HEADs / marker PUTs in flight answer later; uploads that had not
    // started yet are simply dropped.
    for (const auto& a : active) {
        if (!a->uploader)
            m_active.removeOne(a);
    }
    maybeExecuted_();
}

void UploadJob::verify()
{
    // Every upload was checked by its Uploader already.
    end_();
}

Job::Report UploadJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped + m_scan.skippedCount;
    r.failed = m_failed;
    r.failures = m_failures;
    r.notes = m_notes;
    if (m_files + m_markers == 0 && m_failed == 0)
        r.verification = tr("nothing to upload");
    else if (m_sizeVerified > 0)
        r.verification = tr("%1 verified by MD5, %2 by size only (the server's ETag is no MD5 for them)")
                             .arg(m_md5Verified)
                             .arg(m_sizeVerified);
    else
        r.verification = tr("%n file(s) verified by MD5", nullptr, int(m_md5Verified));
    r.verified = m_failed == 0;
    return r;
}

} // namespace qbucket
