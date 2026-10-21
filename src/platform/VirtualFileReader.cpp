#include "platform/VirtualFileReader.h"

#include "controllers/LocalPathMapper.h"

#include <qbucket/s3/ListObjectsPager.h>
#include <qbucket/s3/Session.h>

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <cstring>

namespace qbucket {

namespace {

// "a/b/" → "a/", "a/b.txt" → "a/", "b" → "".
QString parentOf(QString key)
{
    if (key.endsWith(u'/'))
        key.chop(1);
    const qsizetype cut = key.lastIndexOf(u'/');
    return cut < 0 ? QString() : key.left(cut + 1);
}

std::u16string toU16(const QString& s)
{
    return std::u16string(reinterpret_cast<const char16_t*>(s.utf16()), size_t(s.size()));
}

int depthOf(const QString& path)
{
    return int(path.count(u'/'));
}

} // namespace

VirtualFileReader::VirtualFileReader(s3::Session* session, DragStaging::Selection selection)
    : m_session(session),
      m_selection(std::move(selection))
{
    m_thread.setObjectName(QStringLiteral("virtual-file-reader"));
    m_ctx = new QObject;
    m_ctx->moveToThread(&m_thread);
    m_thread.start();
}

VirtualFileReader::~VirtualFileReader()
{
    cancel();
    // The pager lives on the worker: dropped there before it ends.
    QMetaObject::invokeMethod(m_ctx, [this] { m_pager.reset(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_ctx;   // the thread has ended: no deleteLater (lesson in CLAUDE.md)
}

bool VirtualFileReader::onGuiThread_() const
{
    return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread();
}

VirtualFileSource::Status VirtualFileReader::fail_(const QString& why)
{
    m_error = why;
    return Status::Failed;
}

void VirtualFileReader::note_(const QString& line)
{
    if (m_summary.size() < 1000)
        m_summary.append(line);
}

void VirtualFileReader::cancel()
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled = true;
    }
    m_wake.notify_all();
    if (m_session && m_ctx)
        QMetaObject::invokeMethod(m_ctx, [this] {
            if (m_pager)
                m_pager->cancel();
        }, Qt::QueuedConnection);
}

std::u16string VirtualFileReader::lastError() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return toU16(m_error);
}

QStringList VirtualFileReader::summary() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_summary;
}

// --- listing (worker thread) ------------------------------------------------------------

void VirtualFileReader::list_()
{
    listNext_(0);
}

void VirtualFileReader::listNext_(int item)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelled)
            return;
    }
    if (item >= m_selection.items.size()) {
        listed_({});
        return;
    }
    s3::Session* session = m_session.data();
    if (!session) {
        listed_(QCoreApplication::translate("VirtualFileReader", "Not connected."));
        return;
    }
    const DragStaging::Item& it = m_selection.items.at(item);
    const QString parent = parentOf(it.key);
    if (!it.folder) {
        session->headObject(m_selection.bucket, it.key, QString(), m_ctx,
                            [this, item, key = it.key, parent](const s3::Result<s3::ObjectMeta>& head) {
            if (!head) {
                listed_(QCoreApplication::translate("VirtualFileReader", "“%1” could not be read: %2")
                            .arg(key, head.error().toString()));
                return;
            }
            Entry e;
            e.key = key;
            e.path = key.mid(parent.size());
            e.size = head->size;
            e.etag = head->etag;
            e.modified = head->lastModified;
            e.headed = true;
            e.sse = !head->sseCustomerAlgorithm.isEmpty() ? QStringLiteral("SSE-C") : head->serverSideEncryption;
            m_found.push_back(e);
            listNext_(item + 1);
        });
        return;
    }
    // A folder: everything under it, page by page.
    Entry folder;
    folder.key = it.key;
    folder.path = it.key.mid(parent.size());
    folder.folder = true;
    m_found.push_back(folder);
    s3::ListObjectsParams params;
    params.bucket = m_selection.bucket;
    params.prefix = it.key;
    m_pager = std::make_unique<s3::ListObjectsPager>(*session, params);
    fetchPage_(item, parent);
}

void VirtualFileReader::fetchPage_(int item, const QString& parent)
{
    if (!m_pager)
        return;
    m_pager->fetchNext(m_ctx, [this, item, parent](const s3::Result<s3::ListPage>& r) {
        if (!r) {
            listed_(QCoreApplication::translate("VirtualFileReader", "The folder could not be listed: %1")
                        .arg(r.error().toString()));
            return;
        }
        for (const s3::ObjectEntry& o : r->objects) {
            Entry e;
            e.key = o.key;
            e.path = o.key.mid(parent.size());
            e.folder = o.key.endsWith(u'/');
            e.size = o.size;
            e.etag = o.etag;
            e.modified = o.lastModified;
            m_found.push_back(e);
        }
        if (m_found.size() > size_t(kMaxEntries)) {
            listed_(QCoreApplication::translate("VirtualFileReader",
                                                "More than %1 objects: too many to drag. Download them instead.")
                        .arg(kMaxEntries));
            return;
        }
        if (m_pager && m_pager->hasMore()) {
            fetchPage_(item, parent);
            return;
        }
        m_pager.reset();
        listNext_(item + 1);
    });
}

void VirtualFileReader::listed_(const QString& error)
{
    std::vector<Entry> dirs;
    std::vector<Entry> files;
    QString why = error;
    if (why.isEmpty()) {
        QSet<QString> seenDirs;
        QSet<QString> seenNames;
        const auto addDir = [&](const QString& path) {
            if (seenDirs.contains(path))
                return;
            seenDirs.insert(path);
            Entry d;
            d.path = path;
            d.folder = true;
            dirs.push_back(d);
        };
        for (Entry e : m_found) {
            const LocalPathMapper::Result mapped = LocalPathMapper::map(e.path, LocalPathMapper::Rules::Windows);
            if (!mapped.ok) {
                why = QCoreApplication::translate("VirtualFileReader", "“%1” can't be saved on Windows: %2")
                          .arg(e.key, mapped.reason);
                break;
            }
            e.path = mapped.path;
            // Every folder on the way, before what is in it.
            const QStringList parts = e.path.split(u'/', Qt::SkipEmptyParts);
            QString dir;
            for (int i = 0; i < parts.size() - (e.folder ? 0 : 1); ++i) {
                dir += parts.at(i) + u'/';
                addDir(dir);
            }
            if (e.folder)
                continue;
            const QString name = LocalPathMapper::normalized(e.path, LocalPathMapper::Rules::Windows);
            if (seenNames.contains(name)) {
                why = QCoreApplication::translate("VirtualFileReader",
                                                  "“%1” differs from another name only in case: Windows can't hold both.")
                          .arg(e.key);
                break;
            }
            seenNames.insert(name);
            files.push_back(e);
        }
        if (why.isEmpty() && dirs.size() + files.size() > size_t(kMaxEntries))
            why = QCoreApplication::translate("VirtualFileReader",
                                              "More than %1 objects: too many to drag. Download them instead.")
                      .arg(kMaxEntries);
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_found.clear();
        if (!why.isEmpty()) {
            m_listState = ListState::Failed;
            m_error = why;
        } else {
            std::sort(dirs.begin(), dirs.end(), [](const Entry& a, const Entry& b) {
                return depthOf(a.path) != depthOf(b.path) ? depthOf(a.path) < depthOf(b.path) : a.path < b.path;
            });
            std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
            m_entries = dirs;
            m_entries.insert(m_entries.end(), files.begin(), files.end());
            m_listState = ListState::Done;
        }
    }
    m_wake.notify_all();
}

VirtualFileSource::Status VirtualFileReader::files(std::vector<File>* out)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (onGuiThread_())
        return fail_(QStringLiteral("internal: VirtualFileReader::files on the GUI thread"));
    if (m_listState == ListState::NotStarted) {
        m_listState = ListState::Listing;
        QMetaObject::invokeMethod(m_ctx, [this] { list_(); }, Qt::QueuedConnection);
    }
    m_wake.wait(lock, [this] { return m_cancelled || m_listState == ListState::Done || m_listState == ListState::Failed; });
    if (m_cancelled)
        return Status::Cancelled;
    if (m_listState == ListState::Failed)
        return Status::Failed;
    out->clear();
    for (const Entry& e : m_entries) {
        File f;
        QString path = e.path;
        if (path.endsWith(u'/'))
            path.chop(1);
        f.path = toU16(path.replace(u'/', u'\\'));
        f.folder = e.folder;
        f.size = e.folder ? 0 : e.size;
        f.modifiedMsecs = e.modified.isValid() ? e.modified.toMSecsSinceEpoch() : 0;
        out->push_back(f);
    }
    return Status::Ok;
}

// --- reading -----------------------------------------------------------------------------

void VirtualFileReader::head_(std::size_t index)
{
    s3::Session* session = m_session.data();
    QString key;
    QString etag;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        key = m_entries[index].key;
        etag = m_entries[index].etag;
    }
    if (!session) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_files[index].headDone = true;
        m_files[index].headError = QCoreApplication::translate("VirtualFileReader", "Not connected.");
        m_wake.notify_all();
        return;
    }
    s3::HeadObjectParams p;
    p.bucket = m_selection.bucket;
    p.key = key;
    p.ifMatch = etag;
    session->headObject(p, m_ctx, [this, index](const s3::Result<s3::ObjectMeta>& head) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            FileState& f = m_files[index];
            f.headDone = true;
            if (!head) {
                f.headError = head.error().category == s3::ErrorCategory::PreconditionFailed
                    ? QCoreApplication::translate("VirtualFileReader", "changed on the server since the drag started")
                    : head.error().toString();
            } else {
                Entry& e = m_entries[index];
                e.headed = true;
                e.sse = !head->sseCustomerAlgorithm.isEmpty() ? QStringLiteral("SSE-C") : head->serverSideEncryption;
            }
        }
        m_wake.notify_all();
    });
}

void VirtualFileReader::fetch_(std::size_t index, qint64 chunk)
{
    s3::Session* session = m_session.data();
    s3::GetObjectParams p;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const Entry& e = m_entries[index];
        p.bucket = m_selection.bucket;
        p.key = e.key;
        p.ifMatch = e.etag;
        p.rangeStart = chunk * m_chunk;
        p.rangeLength = std::min(m_chunk, e.size - p.rangeStart);
    }
    if (!session) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        Chunk& c = m_files[index].chunks[chunk];
        c.done = true;
        c.error = QCoreApplication::translate("VirtualFileReader", "Not connected.");
        m_wake.notify_all();
        return;
    }
    session->getObject(p, m_ctx, [this, index, chunk](const s3::Result<s3::ObjectData>& r) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto f = m_files.find(index);
            if (f == m_files.end())
                return;
            Chunk& c = f->second.chunks[chunk];
            c.done = true;
            if (r)
                c.data = r->data;
            else
                c.error = r.error().category == s3::ErrorCategory::PreconditionFailed
                    ? QCoreApplication::translate("VirtualFileReader", "changed on the server since the drag started")
                    : r.error().toString();
        }
        m_wake.notify_all();
    });
}

VirtualFileSource::Status VirtualFileReader::read(std::size_t index, std::int64_t offset, void* buffer,
                                                  std::uint32_t length, std::uint32_t* read)
{
    *read = 0;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (onGuiThread_())
        return fail_(QStringLiteral("internal: VirtualFileReader::read on the GUI thread"));
    if (m_cancelled)
        return Status::Cancelled;
    if (m_listState != ListState::Done || index >= m_entries.size() || m_entries[index].folder || offset < 0)
        return fail_(QStringLiteral("internal: no such file"));
    const Entry& e = m_entries[index];
    FileState& f = m_files[index];
    if (offset >= e.size || length == 0)
        return Status::Ok;

    // Once per file: what the object is encrypted with decides whether its
    // ETag is an MD5 (and whether it is still the same object).
    if (!f.started) {
        f.started = true;
        if (!e.headed) {
            QMetaObject::invokeMethod(m_ctx, [this, index] { head_(index); }, Qt::QueuedConnection);
            m_wake.wait(lock, [&] { return m_cancelled || f.headDone; });
            if (m_cancelled)
                return Status::Cancelled;
        }
        f.headDone = true;
        if (!f.headError.isEmpty()) {
            note_(QStringLiteral("%1: failed — %2").arg(e.key, f.headError));
            return fail_(QStringLiteral("%1: %2").arg(e.key, f.headError));
        }
        const QString etag = QString(e.etag).remove(u'"');
        f.verify = etag.size() == 32 && !etag.contains(u'-') && !e.sse.startsWith(QLatin1String("aws:kms"))
                   && e.sse != QLatin1String("SSE-C");
        if (f.verify)
            f.md5 = std::make_unique<QCryptographicHash>(QCryptographicHash::Md5);
    }

    const qint64 chunk = offset / m_chunk;
    const qint64 last = (e.size - 1) / m_chunk;
    for (qint64 c = chunk; c <= std::min(last, chunk + kAhead); ++c) {
        if (f.chunks.find(c) == f.chunks.end()) {
            f.chunks[c];   // requested
            QMetaObject::invokeMethod(m_ctx, [this, index, c] { fetch_(index, c); }, Qt::QueuedConnection);
        }
    }
    m_wake.wait(lock, [&] { return m_cancelled || f.chunks[chunk].done; });
    if (m_cancelled)
        return Status::Cancelled;
    Chunk& c = f.chunks[chunk];
    if (!c.error.isEmpty()) {
        note_(QStringLiteral("%1: failed — %2").arg(e.key, c.error));
        return fail_(QStringLiteral("%1: %2").arg(e.key, c.error));
    }
    const qint64 within = offset - chunk * m_chunk;
    if (within >= c.data.size()) {
        note_(QStringLiteral("%1: failed — short read").arg(e.key));
        return fail_(QStringLiteral("%1: the server sent less than expected").arg(e.key));
    }
    const qint64 n = std::min<qint64>(length, c.data.size() - within);
    const char* bytes = c.data.constData() + within;
    const bool reachesEnd = offset + n == e.size;
    if (f.verify) {
        if (offset == f.hashed) {
            f.md5->addData(QByteArrayView(bytes, n));
            f.hashed += n;
        } else if (offset > f.hashed) {
            f.verify = false;   // skipped ahead: only the size can be told
        }
    }
    if (reachesEnd && !f.finished) {
        f.finished = true;
        if (f.verify && f.hashed == e.size) {
            const QByteArray want = QString(e.etag).remove(u'"').toLatin1().toLower();
            if (f.md5->result().toHex() != want) {
                note_(QStringLiteral("%1: failed — the content does not match its ETag").arg(e.key));
                return fail_(QStringLiteral("%1: the content does not match its ETag").arg(e.key));
            }
            note_(QStringLiteral("%1: md5 verified").arg(e.key));
        } else {
            note_(QStringLiteral("%1: size verified").arg(e.key));
        }
    }
    std::memcpy(buffer, bytes, size_t(n));
    *read = std::uint32_t(n);
    // What was read is not needed again (sequential); keep the current one.
    for (auto it = f.chunks.begin(); it != f.chunks.end() && it->first < chunk;)
        it = f.chunks.erase(it);
    return Status::Ok;
}

} // namespace qbucket
