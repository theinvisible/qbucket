#include "controllers/LocalScanner.h"

#include "controllers/PlanFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTimeZone>

#include <algorithm>
#include <vector>

namespace qbucket {

namespace {

struct Entry {
    QByteArray order;
    QString name;
    QString path;
    bool folder = false;
};

class Walker {
public:
    Walker(QFile& out, const std::atomic<bool>& cancel, std::atomic<qint64>& found, LocalScanner::Result& r)
        : m_out(out), m_cancel(cancel), m_found(found), m_r(r)
    {
    }

    void skip(const QString& path, const QString& reason)
    {
        ++m_r.skippedCount;
        if (m_r.skipped.size() < LocalScanner::kMaxListed)
            m_r.skipped.append(QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(path), reason));
    }

    bool keyFits(const QString& key, const QString& path)
    {
        if (key.toUtf8().size() <= LocalScanner::kMaxKeyBytes)
            return true;
        skip(path, QCoreApplication::translate("qbucket::LocalScanner", "the name is too long for S3 (1024 bytes)"));
        return false;
    }

    void file(const QFileInfo& info, const QString& key, int source)
    {
        if (!keyFits(key, info.filePath()))
            return;
        write({key, info.filePath(), QString::number(info.size()),
               QString::number(info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch()), QStringLiteral("f"),
               QString::number(source)});
        ++m_r.files;
        m_r.bytes += info.size();
        m_found.fetch_add(1, std::memory_order_relaxed);
    }

    void marker(const QString& path, const QString& key, int source)
    {
        if (!keyFits(key, path))
            return;
        write({key, path, QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("d"), QString::number(source)});
        ++m_r.folders;
    }

    // key: the folder's prefix, with the trailing "/".
    void folder(const QString& path, const QString& key, int source)
    {
        if (m_cancel.load(std::memory_order_relaxed) || !m_r.error.isEmpty())
            return;
        const QDir dir(path);
        if (!dir.isReadable()) {
            skip(path, QCoreApplication::translate("qbucket::LocalScanner", "the folder cannot be read"));
            return;
        }
        // One folder's names in memory — sorted, then visited.
        std::vector<Entry> entries;
        QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            Entry e;
            e.name = info.fileName();
            e.path = info.filePath();
            e.folder = info.isDir() && !info.isSymLink();
            e.order = LocalScanner::sortKey(e.name, e.folder);
            entries.push_back(std::move(e));
        }
        if (entries.empty()) {
            marker(path, key, source);
            return;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
        for (const Entry& e : entries) {
            if (m_cancel.load(std::memory_order_relaxed) || !m_r.error.isEmpty())
                return;
            visit(e.path, key + e.name, source);
        }
    }

    // One source or directory entry; key without a trailing "/".
    void visit(const QString& path, const QString& key, int source)
    {
        const QFileInfo info(path);
        if (info.isSymLink() && info.isDir()) {
            skip(path, QCoreApplication::translate("qbucket::LocalScanner", "a link to a folder is not followed"));
            return;
        }
        if (info.isDir()) {
            folder(path, key + u'/', source);
            return;
        }
        if (!info.exists()) {
            skip(path, QCoreApplication::translate("qbucket::LocalScanner", "not found"));
            return;
        }
        if (!info.isFile()) {
            skip(path, QCoreApplication::translate("qbucket::LocalScanner", "not a regular file"));
            return;
        }
        if (!info.isReadable()) {
            skip(path, QCoreApplication::translate("qbucket::LocalScanner", "the file cannot be read"));
            return;
        }
        file(info, key, source);
    }

private:
    void write(const QStringList& fields)
    {
        m_buffer += PlanFile::encodeRecord(fields);
        if (m_buffer.size() >= qsizetype(256) * 1024)
            flush();
    }

public:
    void flush()
    {
        if (m_buffer.isEmpty() || !m_r.error.isEmpty())
            return;
        if (m_out.write(m_buffer) != m_buffer.size())
            m_r.error = m_out.errorString();
        m_buffer.clear();
    }

private:
    QFile& m_out;
    const std::atomic<bool>& m_cancel;
    std::atomic<qint64>& m_found;
    LocalScanner::Result& m_r;
    QByteArray m_buffer;
};

} // namespace

QByteArray LocalScanner::sortKey(const QString& name, bool folder)
{
    QByteArray k = name.toUtf8();
    if (folder)
        k += '/';
    return k;
}

LocalScanner::Result LocalScanner::scan(const QStringList& sources, const QString& prefix, const QString& planPath,
                                        const std::atomic<bool>& cancel, std::atomic<qint64>& found)
{
    Result r;
    QDir().mkpath(QFileInfo(planPath).absolutePath());
    QFile out(planPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.error = out.errorString();
        return r;
    }
    Walker walker(out, cancel, found, r);

    // Sources in key order too; two of the same name would be one key.
    std::vector<Entry> roots;
    for (const QString& s : sources) {
        const QFileInfo info(s);
        Entry e;
        e.path = QDir::cleanPath(info.absoluteFilePath());
        e.name = info.fileName();
        e.folder = info.isDir() && !info.isSymLink();
        e.order = sortKey(e.name, e.folder);
        roots.push_back(std::move(e));
    }
    std::stable_sort(roots.begin(), roots.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
    QByteArray previous;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (cancel.load())
            break;
        const Entry& e = roots[i];
        if (e.name.isEmpty()) {
            walker.skip(e.path, QCoreApplication::translate("qbucket::LocalScanner", "a drive or root folder cannot be uploaded as a whole"));
            continue;
        }
        // Two files (or two folders) of one name would be the same keys. A
        // file "a" and a folder "a" are fine: "a" and "a/…" are different keys.
        if (i > 0 && e.order == previous) {
            walker.skip(e.path, QCoreApplication::translate("qbucket::LocalScanner",
                                                            "another file or folder of this name is part of the upload"));
            continue;
        }
        previous = e.order;
        walker.visit(e.path, prefix + e.name, int(i));
    }
    walker.flush();
    r.cancelled = cancel.load();
    if (r.error.isEmpty() && !out.flush())
        r.error = out.errorString();
    return r;
}

} // namespace qbucket
