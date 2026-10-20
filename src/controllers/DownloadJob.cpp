#include "controllers/DownloadJob.h"

#include "controllers/AccountSessions.h"
#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"

#include <qbucket/s3/transfer/Downloader.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QPointer>
#include <QThreadPool>
#include <QTimeZone>

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

QString lastSegment(QString key)
{
    while (key.endsWith(u'/'))
        key.chop(1);
    return key.mid(key.lastIndexOf(u'/') + 1);
}

bool archived(const QString& storageClass)
{
    return storageClass == QLatin1String("GLACIER") || storageClass == QLatin1String("DEEP_ARCHIVE");
}

} // namespace

DownloadJob::Record DownloadJob::Record::from(const QStringList& f)
{
    Record r;
    r.key = f.value(0);
    r.path = f.value(1);
    r.size = f.value(2).toLongLong();
    r.etag = f.value(3);
    r.mtimeMs = f.value(4).toLongLong();
    r.localState = f.value(5);
    r.localSize = f.value(6).toLongLong();
    r.localMtimeMs = f.value(7).toLongLong();
    return r;
}

DownloadJob::DownloadJob(const Account& account, const QString& bucket, const QList<Item>& items,
                         const QString& targetDir, const QString& planDir, Options options, QObject* parent)
    : Job(account, bucket, parent),
      m_items(items),
      m_target(QDir::cleanPath(targetDir)),
      m_planDir(planDir),
      m_options(options)
{
}

DownloadJob::~DownloadJob()
{
    if (m_checkCancel)
        m_checkCancel->store(true);
    m_active.clear();
}

QString DownloadJob::title() const
{
    if (m_items.size() == 1)
        return tr("Download “%1” to %2").arg(lastSegment(m_items.first().key), QDir::toNativeSeparators(m_target));
    return tr("Download %n item(s) to %1", nullptr, int(m_items.size()))
        .arg(QDir::toNativeSeparators(m_target));
}

bool DownloadJob::needsConfirmation() const
{
    return m_options.alwaysConfirm || m_check.conflicts > 0 || m_check.skipped > 0 || m_check.renamed > 0
        || m_check.files + m_check.folders != 1 || account().environment == Environment::Production;
}

QList<Job::ActiveItem> DownloadJob::activeItems() const
{
    QList<ActiveItem> out;
    for (const auto& a : m_active)
        out.append({a->record.key, a->downloader ? a->downloader->bytesDone() : 0, a->record.size});
    return out;
}

QString DownloadJob::absolute_(const QString& relative) const
{
    return m_target + u'/' + relative;
}

// --- the worker pass ------------------------------------------------------------

DownloadJob::LocalCheck DownloadJob::checkLocal(const QString& rawPath, const QString& finalPath,
                                                const QString& targetDir, LocalPathMapper::Rules rules,
                                                const std::atomic<bool>& cancel)
{
    LocalCheck c;
    QFile in(rawPath);
    QFile out(finalPath);
    if (!in.open(QIODevice::ReadOnly)) {
        c.error = in.errorString();
        return c;
    }
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        c.error = out.errorString();
        return c;
    }
    const auto note = [&c](const QString& key, const QString& why) {
        ++c.skipped;
        if (c.notes.size() < kMaxListed)
            c.notes.append(QStringLiteral("%1: %2").arg(key, why));
    };
    // What this download writes, as the file system compares names:
    // 'f' a file, 'd' a folder.
    QHash<QString, char> seen;
    QByteArray buffer;
    while (!in.atEnd()) {
        if (cancel.load(std::memory_order_relaxed))
            return c;
        const QStringList f = PlanFile::decodeRecord(in.readLine());
        const QString key = f.value(0);
        if (archived(f.value(5))) {
            note(key, tr("archived (storage class %1) — restore it first").arg(f.value(5)));
            continue;
        }
        const LocalPathMapper::Result mapped = LocalPathMapper::map(f.value(1), rules);
        if (!mapped.ok) {
            note(key, mapped.reason);
            continue;
        }
        const bool folder = mapped.path.endsWith(u'/');
        const QString path = folder ? mapped.path.chopped(1) : mapped.path;
        const QString norm = LocalPathMapper::normalized(path, rules);
        // Collisions within the download: "a" as a file and as a folder,
        // or two keys that end up as one name here.
        bool collides = false;
        QString parent = norm;
        while (true) {
            const qsizetype slash = parent.lastIndexOf(u'/');
            if (slash < 0)
                break;
            parent = parent.left(slash);
            if (seen.value(parent, 'd') == 'f') {
                collides = true;
                break;
            }
        }
        const char already = seen.value(norm, 0);
        if (collides || (already != 0 && (!folder || already == 'f'))) {
            note(key, tr("another object of this download has the same local name"));
            continue;
        }
        parent = norm;
        while (true) {
            const qsizetype slash = parent.lastIndexOf(u'/');
            if (slash < 0)
                break;
            parent = parent.left(slash);
            seen.insert(parent, 'd');
        }
        seen.insert(norm, folder ? 'd' : 'f');

        const QFileInfo local(targetDir + u'/' + path);
        QString state;
        qint64 localSize = 0;
        qint64 localMtime = 0;
        if (local.exists()) {
            if (folder) {
                if (!local.isDir())
                    note(key, tr("a file of this name exists here"));
                continue;   // the folder is there already: nothing to do
            }
            if (local.isDir()) {
                note(key, tr("a folder of this name exists here"));
                continue;
            }
            state = QStringLiteral("file");
            localSize = local.size();
            localMtime = local.lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
            ++c.conflicts;
            if (c.conflictSamples.size() < kConflictSamples) {
                c.conflictSamples.append(tr("%1 — here %2; there %3")
                                             .arg(path, describeSide(localSize, localMtime),
                                                  describeSide(f.value(2).toLongLong(), f.value(4).toLongLong())));
            }
        }
        if (mapped.changed) {
            ++c.renamed;
            if (c.renamedSamples.size() < kSamples)
                c.renamedSamples.append(QStringLiteral("%1 → %2").arg(key, mapped.path));
        }
        if (folder) {
            ++c.folders;
        } else {
            ++c.files;
            c.bytes += f.value(2).toLongLong();
        }
        if (c.samples.size() < kSamples)
            c.samples.append(key);
        buffer += PlanFile::encodeRecord({key, mapped.path, f.value(2), f.value(3), f.value(4), state,
                                          QString::number(localSize), QString::number(localMtime)});
        if (buffer.size() >= qsizetype(256) * 1024) {
            if (out.write(buffer) != buffer.size()) {
                c.error = out.errorString();
                return c;
            }
            buffer.clear();
        }
    }
    const bool written = buffer.isEmpty() || out.write(buffer) == buffer.size();
    if (!written || !out.flush())
        c.error = out.errorString();
    return c;
}

// --- plan -----------------------------------------------------------------------

void DownloadJob::plan()
{
    m_run = std::make_unique<QObject>();
    m_raw = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".list"));
    m_planIndex = 0;
    planNext_();
}

void DownloadJob::planNext_()
{
    if (phase() != Phase::Planning)
        return;
    // Objects first (HEAD, a few at a time), then each folder, page by page.
    while (m_planIndex < m_items.size() && !m_items[m_planIndex].folder) {
        if (m_headsInFlight >= kParallelHeads)
            return;
        const Item item = m_items[m_planIndex++];
        s3::Session* session = session_();
        if (!session) {
            fail_(tr("Not connected."));
            return;
        }
        ++m_headsInFlight;
        session->headObject(bucket(), item.key, QString(), m_run.get(), [this, item](const s3::Result<s3::ObjectMeta>& head) {
            --m_headsInFlight;
            if (phase() != Phase::Planning)
                return;
            if (head) {
                const QString rel = item.localName.isEmpty() ? lastSegment(item.key) : item.localName;
                m_raw->appendRecords({{item.key, rel, QString::number(head->size), head->etag,
                                       QString::number(head->lastModified.toMSecsSinceEpoch()), head->storageClass}});
            } else if (head.error().category == s3::ErrorCategory::NotFound) {
                ++m_missing;
            } else {
                fail_(tr("Could not look up “%1”: %2").arg(item.key, ConnectionTester::describe(head.error())));
                return;
            }
            planNext_();
        });
    }
    if (m_headsInFlight > 0)
        return;
    if (m_planIndex < m_items.size()) {
        planFolder_(m_items[m_planIndex]);
        return;
    }
    planDone_();
}

void DownloadJob::planFolder_(const Item& item)
{
    if (!m_pager) {
        s3::Session* session = session_();
        if (!session) {
            fail_(tr("Not connected."));
            return;
        }
        s3::ListObjectsParams params;
        params.bucket = bucket();
        params.prefix = item.key;
        params.maxKeys = 1000;
        m_pager = std::make_unique<s3::ListObjectsPager>(*session, params);
    }
    m_pager->fetchNext(m_run.get(), [this, item](const s3::Result<s3::ListPage>& page) {
        if (phase() != Phase::Planning)
            return;
        if (!page) {
            fail_(tr("Could not list “%1”: %2").arg(item.key, ConnectionTester::describe(page.error())));
            return;
        }
        const QString base = (item.localName.isEmpty() ? lastSegment(item.key) : item.localName) + u'/';
        QList<QStringList> records;
        records.reserve(page->objects.size());
        for (const s3::ObjectEntry& e : page->objects) {
            records.append({e.key, base + e.key.mid(item.key.size()), QString::number(e.size), e.etag,
                            QString::number(e.lastModified.toMSecsSinceEpoch()), e.storageClass});
        }
        m_raw->appendRecords(records);
        setProgress_(m_raw->count(), 0);
        if (m_pager->hasMore()) {
            planFolder_(item);
            return;
        }
        m_pager.reset();
        ++m_planIndex;
        planNext_();
    });
}

void DownloadJob::planDone_()
{
    m_raw->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_raw->errorText()));
            return;
        }
        // Map and check the local side on a worker: a stat per object.
        m_checkCancel = std::make_shared<std::atomic<bool>>(false);
        const QPointer<DownloadJob> self(this);
        auto* relay = new QObject;
        relay->moveToThread(thread());
        QThreadPool::globalInstance()->start([relay, self, raw = m_raw->path(),
                                              out = m_planDir + u'/' + id() + QStringLiteral(".plan"), target = m_target,
                                              cancel = m_checkCancel] {
            const LocalCheck c = checkLocal(raw, out, target, LocalPathMapper::nativeRules(), *cancel);
            QMetaObject::invokeMethod(relay, [relay, self, c] {
                if (self)
                    self->checked_(c);
                relay->deleteLater();
            }, Qt::QueuedConnection);
        });
    });
}

void DownloadJob::checked_(const LocalCheck& check)
{
    if (phase() != Phase::Planning)
        return;
    if (!check.error.isEmpty()) {
        fail_(tr("The plan could not be written: %1").arg(check.error));
        return;
    }
    m_check = check;
    m_raw.reset();
    m_notes = check.notes;
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"), PlanFile::Mode::Existing,
                                        check.files + check.folders);
    if (check.files + check.folders == 0) {
        finish_(report_());
        return;
    }
    Preview p;
    p.action = tr("Download");
    p.scope = m_items.size() == 1
        ? QStringLiteral("s3://%1/%2 → %3").arg(bucket(), m_items.first().key, QDir::toNativeSeparators(m_target))
        : tr("s3://%1 — %n selected item(s) → %2", nullptr,
                                      int(m_items.size()))
              .arg(bucket(), QDir::toNativeSeparators(m_target));
    p.objects = check.files + check.folders;
    p.bytes = check.bytes;
    p.samples = check.samples;
    if (m_missing > 0)
        p.details.append(tr("%n selected object(s) no longer exist and are skipped.", nullptr,
                                                     int(m_missing)));
    if (check.renamed > 0) {
        p.warnings.append(tr("%n name(s) contain characters this system does not allow and "
                                                      "are saved under another name, e.g. %1",
                                                      nullptr, int(check.renamed))
                              .arg(check.renamedSamples.value(0)));
    }
    if (check.skipped > 0) {
        p.warnings.append(tr("%n object(s) can't be saved here and are left out — first: %1",
                                                      nullptr, int(check.skipped))
                              .arg(check.notes.value(0)));
    }
    p.conflicts = check.conflicts;
    p.conflictSamples = check.conflictSamples;
    if (check.conflicts > 0)
        p.warnings.append(tr("Overwriting replaces those local files; the previous content is not kept."));
    setProgress_(0, p.objects);
    setBytes_(0, check.bytes);
    setPreview_(p);
}

// --- execute --------------------------------------------------------------------

void DownloadJob::execute()
{
    m_progressClock.start();
    setProgress_(0, m_check.files + m_check.folders);
    setBytes_(0, m_check.bytes);
    pump_();
}

void DownloadJob::pump_()
{
    if (phase() != Phase::Executing)
        return;
    if (isCancelled() || isPaused()) {
        maybeExecuted_();
        return;
    }
    while (m_active.size() < kParallelFiles && !m_queue.empty()) {
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
            for (const QStringList& f : records)
                m_queue.push_back(Record::from(f));
            pump_();
        });
    }
    maybeExecuted_();
}

void DownloadJob::maybeExecuted_()
{
    if (phase() != Phase::Executing || !m_active.isEmpty() || m_readingPlan)
        return;
    if (isCancelled()) {
        updateProgress_(true);
        finish_(report_());
        return;
    }
    if (isPaused())
        return;
    if (m_planExhausted && m_queue.empty()) {
        updateProgress_(true);
        executed_();
    }
}

void DownloadJob::startItem_(const Record& r)
{
    if (r.path.endsWith(u'/')) {
        // A folder marker: the folder.
        if (QDir().mkpath(absolute_(r.path))) {
            ++m_succeeded;
            ++m_processed;
        } else {
            itemFailed_(r, tr("the folder could not be created"));
        }
        updateProgress_(false);
        return;
    }
    auto item = std::make_shared<Active>();
    item->record = r;
    item->path = r.path;
    if (r.localState == QLatin1String("file")) {
        switch (conflictPolicy()) {
        case ConflictPolicy::Skip:
            itemSkipped_(r.key, tr("exists here already"));
            return;
        case ConflictPolicy::Overwrite:
            m_active.append(item);
            download_(item, true);
            return;
        case ConflictPolicy::KeepBoth:
            for (item->keepBothTry = 1; item->keepBothTry < 100; ++item->keepBothTry) {
                item->path = UploadJob::keepBothName(r.path, item->keepBothTry);
                if (!QFileInfo::exists(absolute_(item->path)))
                    break;
            }
            break;
        }
    }
    m_active.append(item);
    download_(item, false);
}

void DownloadJob::download_(const std::shared_ptr<Active>& item, bool overwrite)
{
    s3::Session* session = session_();
    TransferService* transfers = controller_()->transfers();
    if (!session || !transfers) {
        m_active.removeOne(item);
        itemFailed_(item->record, tr("Not connected."));
        pump_();
        return;
    }
    s3::transfer::DownloadRequest req;
    req.bucket = bucket();
    req.key = item->record.key;
    req.targetPath = absolute_(item->path);
    req.overwrite = overwrite;
    req.expectEtag = item->record.etag;   // what the plan saw
    req.transferId = QStringLiteral("%1-%2").arg(id()).arg(++m_nextTransfer);
    req.account = account().id.toString(QUuid::WithoutBraces);
    req.extra.insert(QStringLiteral("job"), id());
    item->downloader = std::make_unique<s3::transfer::Downloader>(session, req, transfers->limiter(account().id),
                                                                  transfers->resumeStore());
    Active* raw = item.get();
    connect(item->downloader.get(), &s3::transfer::Downloader::finished, this, [this, raw] { itemDone_(raw); });
    connect(item->downloader.get(), &s3::transfer::Downloader::progress, this, [this] { updateProgress_(false); });
    item->downloader->start();
    if (isPaused())
        item->downloader->pause();
}

void DownloadJob::itemDone_(Active* raw)
{
    std::shared_ptr<Active> item;
    for (const auto& a : std::as_const(m_active)) {
        if (a.get() == raw)
            item = a;
    }
    if (!item || phase() != Phase::Executing)
        return;
    m_active.removeOne(item);
    const s3::Result<s3::transfer::FileDownloadResult>& r = item->downloader->result();
    Journal::Record rec = record_(s3::operationName(s3::Operation::GetObject), item->record.key);
    if (r) {
        rec.httpStatus = 200;
        rec.versionId = r->versionId;
        rec.message = QStringLiteral("%1 → %2, %3").arg(r->etag, r->path,
                                                       r->integrity == s3::Integrity::Md5Verified
                                                           ? QStringLiteral("md5 verified")
                                                           : QStringLiteral("size verified"));
        journal_({rec});
        ++m_succeeded;
        ++m_processed;
        m_doneBytes += item->record.size;
        if (r->integrity == s3::Integrity::Md5Verified)
            ++m_md5Verified;
        else
            ++m_sizeVerified;
    } else {
        const s3::Error& e = r.error();
        if (e.category == s3::ErrorCategory::Cancelled) {
            ++m_processed;
        } else if (e.category == s3::ErrorCategory::Conflict) {
            // A local file appeared after the plan: never overwritten unasked.
            if (conflictPolicy() == ConflictPolicy::KeepBoth && item->keepBothTry < 100) {
                auto again = std::make_shared<Active>();
                again->record = item->record;
                for (again->keepBothTry = item->keepBothTry + 1; again->keepBothTry < 100; ++again->keepBothTry) {
                    again->path = UploadJob::keepBothName(item->record.path, again->keepBothTry);
                    if (!QFileInfo::exists(absolute_(again->path)))
                        break;
                }
                m_active.append(again);
                download_(again, false);
                item->downloader.release()->deleteLater();
                return;
            }
            itemSkipped_(item->record.key, tr("appeared here meanwhile"));
        } else {
            rec.result = Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message = e.toString();
            journal_({rec});
            itemFailed_(item->record, e.category == s3::ErrorCategory::PreconditionFailed
                                          ? tr("changed on the server since the plan")
                                          : ConnectionTester::describe(e));
        }
    }
    item->downloader.release()->deleteLater();
    updateProgress_(false);
    pump_();
}

void DownloadJob::itemSkipped_(const QString& key, const QString& why)
{
    ++m_skipped;
    ++m_processed;
    if (m_notes.size() < kMaxListed)
        m_notes.append(QStringLiteral("%1: %2").arg(key, why));
    updateProgress_(false);
}

void DownloadJob::itemFailed_(const Record& r, const QString& reason)
{
    ++m_failed;
    ++m_processed;
    if (m_failures.size() < kMaxListed)
        m_failures.append(QStringLiteral("%1: %2").arg(r.key, reason));
    if (!r.path.endsWith(u'/'))
        m_failedItems.append({r.key, false, r.path});
    updateProgress_(false);
}

void DownloadJob::updateProgress_(bool force)
{
    if (!force && m_progressClock.isValid() && m_progressClock.elapsed() < 100)
        return;
    m_progressClock.start();
    qint64 bytes = m_doneBytes;
    for (const auto& a : std::as_const(m_active)) {
        if (a->downloader)
            bytes += a->downloader->bytesDone();
    }
    setProgress_(m_processed, m_check.files + m_check.folders);
    setBytes_(bytes, m_check.bytes);
}

void DownloadJob::onPause()
{
    for (const auto& a : std::as_const(m_active)) {
        if (a->downloader)
            a->downloader->pause();
    }
}

void DownloadJob::onResume()
{
    for (const auto& a : std::as_const(m_active)) {
        if (a->downloader)
            a->downloader->resume();
    }
    pump_();
}

void DownloadJob::onCancel()
{
    if (m_checkCancel)
        m_checkCancel->store(true);
    if (phase() == Phase::Planning) {
        m_run.reset();
        m_pager.reset();
        return;
    }
    if (phase() != Phase::Executing)
        return;
    m_queue.clear();
    m_planExhausted = true;
    const auto active = m_active;
    for (const auto& a : active) {
        if (a->downloader)
            a->downloader->cancel();   // removes the .qbpart; finished follows
    }
    maybeExecuted_();
}

void DownloadJob::verify()
{
    finish_(report_());
}

Job::Report DownloadJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped + m_check.skipped + m_missing;
    r.failed = m_failed;
    r.failures = m_failures;
    r.notes = m_notes;
    if (m_check.files + m_check.folders == 0 && m_failed == 0)
        r.verification = tr("nothing to download");
    else if (m_sizeVerified > 0)
        r.verification = tr("%1 verified by MD5, %2 by size only (their ETag is no MD5)").arg(m_md5Verified).arg(m_sizeVerified);
    else
        r.verification = tr("%n file(s) verified by MD5", nullptr,
                                                     int(m_md5Verified));
    r.verified = m_failed == 0;
    return r;
}

} // namespace qbucket
