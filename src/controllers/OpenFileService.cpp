#include "controllers/OpenFileService.h"

#include "accounts/AccountStore.h"
#include "controllers/DownloadJob.h"
#include "controllers/OperationController.h"
#include "controllers/ReplaceObjectJob.h"
#include "diagnostics/Logging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>

#include <optional>

namespace qbucket {
namespace {

QString defaultRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/open");
}

QString defaultStore()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/open-files.json");
}

// MD5 (hex), size and mtime of a file on the thread pool; `done` on the
// context's thread (dropped with it). Empty MD5: unreadable.
void hashFile(const QString& path, QObject* context,
              std::function<void(const QByteArray& md5, qint64 size, const QDateTime& modified)> done)
{
    auto* relay = new QObject;
    relay->moveToThread(context->thread());
    const QPointer<QObject> alive(context);
    QThreadPool::globalInstance()->start([relay, alive, path, done = std::move(done)] {
        QByteArray md5;
        const QFileInfo info(path);
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            QCryptographicHash h(QCryptographicHash::Md5);
            if (h.addData(&f))
                md5 = h.result().toHex();
        }
        const qint64 size = info.size();
        const QDateTime modified = info.lastModified();
        QMetaObject::invokeMethod(relay, [relay, alive, done, md5, size, modified] {
            if (alive)
                done(md5, size, modified);
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

void makeWritable(const QString& path)
{
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
}

} // namespace

OpenFileService::OpenFileService(OperationController* operations, QString root, QString store, QObject* parent)
    : QObject(parent),
      m_ops(operations),
      m_root(root.isEmpty() ? defaultRoot() : std::move(root)),
      m_store(store.isEmpty() ? defaultStore() : std::move(store)),
      m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &OpenFileService::pathChanged_);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &OpenFileService::pathChanged_);
    load_();
}

OpenFileService::~OpenFileService() = default;

OpenFileService::Entry* OpenFileService::find_(const QUuid& id)
{
    for (Entry& e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

const OpenFileService::Entry* OpenFileService::entry(const QUuid& id) const
{
    for (const Entry& e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

QList<OpenFileService::Entry> OpenFileService::changedEntries() const
{
    QList<Entry> out;
    for (const Entry& e : m_entries) {
        if (e.state == State::Changed || e.state == State::Uploading)
            out.append(e);
    }
    return out;
}

void OpenFileService::setState_(Entry* e, State state, const QString& error)
{
    e->state = state;
    e->error = error;
    emit entriesChanged();
}

// --- open ------------------------------------------------------------------------------

void OpenFileService::open(const QUuid& account, const QString& bucket, const QString& key, const QString& versionId)
{
    for (const Entry& e : std::as_const(m_entries)) {
        if (e.account == account && e.bucket == bucket && e.key == key && e.versionId == versionId
            && e.state != State::Failed) {
            if (e.state != State::Downloading)
                openAgain(e.id);
            return;
        }
    }
    const std::optional<Account> a = m_ops->accounts()->account(account);
    if (!a)
        return;
    Entry e;
    e.id = QUuid::createUuid();
    e.account = account;
    e.bucket = bucket;
    e.key = key;
    e.versionId = versionId;
    e.readOnly = !versionId.isEmpty() || a->readOnly;
    const QString folder = m_root + u'/' + e.id.toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(folder)) {
        emit message(tr("“%1” could not be opened: the folder %2 could not be created.")
                         .arg(key, QDir::toNativeSeparators(folder)));
        return;
    }
    m_entries.append(e);
    emit entriesChanged();

    DownloadJob::Options options;
    options.transient = true;
    options.confirmAboveBytes = m_confirmAbove;
    options.listDone = true;
    auto* job = new DownloadJob(*a, bucket, {{key, false, {}, versionId}}, folder, m_ops->planDir(), options);
    const QUuid id = e.id;
    connect(job, &Job::finished, this, [this, id, job] { downloaded_(id, job); });
    m_ops->start(job);
}

void OpenFileService::downloaded_(const QUuid& id, Job* job)
{
    Entry* e = find_(id);
    if (!e)
        return;
    const auto* download = qobject_cast<DownloadJob*>(job);
    const QList<DownloadJob::DoneItem> done = download ? download->doneItems() : QList<DownloadJob::DoneItem>();
    if (job->outcome() != Job::Outcome::Succeeded || done.size() != 1) {
        QString why = job->errorText();
        if (why.isEmpty() && !job->report().failures.isEmpty())
            why = job->report().failures.first();
        if (job->outcome() != Job::Outcome::Cancelled)
            emit message(why.isEmpty() ? tr("“%1” could not be opened.").arg(e->key)
                                       : tr("“%1” could not be opened: %2").arg(e->key, why));
        const QString folder = m_root + u'/' + id.toString(QUuid::WithoutBraces);
        m_entries.removeIf([&id](const Entry& x) { return x.id == id; });
        QDir(folder).removeRecursively();
        emit entriesChanged();
        return;
    }
    e->path = done.first().path;
    e->etag = done.first().etag;
    if (e->readOnly)
        QFile::setPermissions(e->path, QFileDevice::ReadOwner | QFileDevice::ReadUser | QFileDevice::ReadGroup
                                           | QFileDevice::ReadOther);
    hashFile(e->path, this, [this, id](const QByteArray& md5, qint64 size, const QDateTime& modified) {
        Entry* x = find_(id);
        if (!x)
            return;
        x->md5 = md5;
        x->size = size;
        x->modified = modified;
        setState_(x, State::Open);
        if (!x->readOnly)
            watch_(*x);
        if (m_opener && !m_opener(QUrl::fromLocalFile(x->path)))
            emit message(tr("No application opened %1.").arg(QDir::toNativeSeparators(x->path)));
    });
}

void OpenFileService::openAgain(const QUuid& id)
{
    const Entry* e = entry(id);
    if (!e || e->path.isEmpty())
        return;
    if (m_opener && !m_opener(QUrl::fromLocalFile(e->path)))
        emit message(tr("No application opened %1.").arg(QDir::toNativeSeparators(e->path)));
}

// --- watch -----------------------------------------------------------------------------

void OpenFileService::watch_(const Entry& e)
{
    // The folder too: editors save by writing another file and renaming
    // it over the original, which ends a watch on the file.
    if (QFileInfo::exists(e.path) && !m_watcher->files().contains(e.path))
        m_watcher->addPath(e.path);
    const QString folder = QFileInfo(e.path).absolutePath();
    if (!m_watcher->directories().contains(folder))
        m_watcher->addPath(folder);
}

void OpenFileService::pathChanged_(const QString& path)
{
    for (const Entry& e : std::as_const(m_entries)) {
        if (e.readOnly || (e.state != State::Open && e.state != State::Changed))
            continue;
        if (e.path != path && QFileInfo(e.path).absolutePath() != path)
            continue;
        QTimer*& timer = m_settle[e.id];
        if (!timer) {
            timer = new QTimer(this);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, this, [this, id = e.id] { settle_(id); });
        }
        timer->start(m_settleMs);
    }
}

void OpenFileService::settle_(const QUuid& id)
{
    Entry* e = find_(id);
    if (!e || e->readOnly)
        return;
    const QFileInfo info(e->path);
    if (!info.exists())
        return;   // mid-save; the folder's next change comes back here
    watch_(*e);
    if (info.size() == e->size && info.lastModified() == e->modified)
        return;
    hashFile(e->path, this, [this, id](const QByteArray& md5, qint64 size, const QDateTime& modified) {
        hashed_(id, md5, size, modified);
    });
}

void OpenFileService::hashed_(const QUuid& id, const QByteArray& md5, qint64 size, const QDateTime& modified)
{
    Entry* e = find_(id);
    if (!e || md5.isEmpty())
        return;
    e->size = size;
    e->modified = modified;
    m_pendingMd5[id] = md5;
    if (e->state == State::Uploading)
        return;
    if (md5 == e->md5) {
        if (e->state == State::Changed)
            setState_(e, State::Open);   // changed back
        return;
    }
    if (e->state != State::Changed) {
        setState_(e, State::Changed);
        emit fileChanged(id);
    }
}

// --- upload ----------------------------------------------------------------------------

void OpenFileService::upload(const QUuid& id)
{
    Entry* e = find_(id);
    if (!e || e->readOnly || e->state != State::Changed)
        return;
    const std::optional<Account> a = m_ops->accounts()->account(e->account);
    if (!a) {
        setState_(e, State::Changed, tr("The account is gone."));
        return;
    }
    const QByteArray md5 = m_pendingMd5.value(id);
    auto* job = new ReplaceObjectJob(*a, e->bucket, {e->key, e->path, e->etag});
    setState_(e, State::Uploading);
    connect(job, &Job::finished, this, [this, id, job, md5] { uploaded_(id, job, md5); });
    m_ops->start(job);
}

void OpenFileService::uploaded_(const QUuid& id, Job* job, const QByteArray& md5)
{
    Entry* e = find_(id);
    if (!e)
        return;
    const auto* replace = qobject_cast<ReplaceObjectJob*>(job);
    if (replace && job->outcome() == Job::Outcome::Succeeded && job->report().succeeded == 1) {
        e->etag = replace->writtenEtag();
        e->key = replace->writtenKey();   // "keep both": the copy is that object now
        e->md5 = md5;
        setState_(e, State::Open);
        saveKept_();
        settle_(id);   // changed again while it was uploaded?
        return;
    }
    QString why = job->errorText();
    if (why.isEmpty() && !job->report().failures.isEmpty())
        why = job->report().failures.first();
    if (why.isEmpty() && !job->report().notes.isEmpty())
        why = job->report().notes.first();
    setState_(e, State::Changed, why.isEmpty() ? tr("Not uploaded.") : why);
}

void OpenFileService::ignoreChange(const QUuid& id)
{
    Entry* e = find_(id);
    if (!e || e->state != State::Changed)
        return;
    if (m_pendingMd5.contains(id))
        e->md5 = m_pendingMd5.value(id);
    setState_(e, State::Open);
    saveKept_();
}

void OpenFileService::discard(const QUuid& id)
{
    Entry* e = find_(id);
    if (!e || e->state == State::Uploading || e->state == State::Downloading)
        return;
    if (!e->path.isEmpty()) {
        m_watcher->removePath(e->path);
        m_watcher->removePath(QFileInfo(e->path).absolutePath());
        makeWritable(e->path);
    }
    QDir(m_root + u'/' + id.toString(QUuid::WithoutBraces)).removeRecursively();
    delete m_settle.take(id);
    m_pendingMd5.remove(id);
    m_entries.removeIf([&id](const Entry& x) { return x.id == id; });
    saveKept_();
    emit entriesChanged();
}

// --- quitting, the next session ---------------------------------------------------------

void OpenFileService::keepForLater()
{
    saveKept_();
    cleanup();
}

void OpenFileService::cleanup()
{
    const QList<Entry> all = m_entries;
    for (const Entry& e : all) {
        if (e.state == State::Open || e.state == State::Failed)
            discard(e.id);
    }
    // Stray folders of earlier sessions: only uuid-named ones, never more.
    QSet<QString> keep;
    for (const Entry& e : std::as_const(m_entries))
        keep.insert(e.id.toString(QUuid::WithoutBraces));
    const QDir root(m_root);
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QUuid::fromString(name).isNull() || keep.contains(name))
            continue;
        QDir dir(root.filePath(name));
        for (const QString& f : dir.entryList(QDir::Files | QDir::Hidden))
            makeWritable(dir.filePath(f));
        dir.removeRecursively();
    }
}

void OpenFileService::saveKept_()
{
    QJsonArray kept;
    for (const Entry& e : std::as_const(m_entries)) {
        if (e.state != State::Changed && e.state != State::Uploading)
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("id"), e.id.toString(QUuid::WithoutBraces));
        o.insert(QStringLiteral("account"), e.account.toString(QUuid::WithoutBraces));
        o.insert(QStringLiteral("bucket"), e.bucket);
        o.insert(QStringLiteral("key"), e.key);
        o.insert(QStringLiteral("path"), e.path);
        o.insert(QStringLiteral("etag"), e.etag);
        o.insert(QStringLiteral("md5"), QString::fromLatin1(e.md5));
        kept.append(o);
    }
    if (kept.isEmpty()) {
        QFile::remove(m_store);
        return;
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), 1);
    root.insert(QStringLiteral("changed"), kept);
    QDir().mkpath(QFileInfo(m_store).absolutePath());
    QSaveFile f(m_store);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0 || !f.commit())
        qCWarning(lcApp).noquote() << "open files: cannot write" << m_store << f.errorString();
}

void OpenFileService::load_()
{
    QFile f(m_store);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    for (const auto& v : doc.object().value(QStringLiteral("changed")).toArray()) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.id = QUuid::fromString(o.value(QStringLiteral("id")).toString());
        e.account = QUuid::fromString(o.value(QStringLiteral("account")).toString());
        e.bucket = o.value(QStringLiteral("bucket")).toString();
        e.key = o.value(QStringLiteral("key")).toString();
        e.path = o.value(QStringLiteral("path")).toString();
        e.etag = o.value(QStringLiteral("etag")).toString();
        e.md5 = o.value(QStringLiteral("md5")).toString().toLatin1();
        // Only copies in our own folder that are still there.
        const bool ours = QFileInfo(e.path).absolutePath()
                          == QDir(m_root).filePath(e.id.toString(QUuid::WithoutBraces));
        if (e.id.isNull() || e.account.isNull() || !ours || !QFileInfo::exists(e.path))
            continue;
        const QFileInfo info(e.path);
        e.size = info.size();
        e.modified = info.lastModified();
        e.state = State::Changed;
        m_entries.append(e);
        watch_(e);
        // What the copy holds now: the content an upload would send.
        hashFile(e.path, this, [this, id = e.id](const QByteArray& md5, qint64 size, const QDateTime& modified) {
            hashed_(id, md5, size, modified);
        });
    }
}

} // namespace qbucket
