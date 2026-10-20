#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>

namespace qbucket {

// Walks local files and folders for an upload (PLAN.md §13.2): one plan
// record per file and per empty folder, streamed into a plan file — memory
// stays bounded for a million files. Siblings are visited in S3's key order
// (UTF-8 bytes of the name, a folder's with its "/"), so the records come
// out sorted by key and can be merged with a listing of the target.
//
// Record fields (PlanFile::encodeRecord): key, local path, size, mtime (ms
// since the epoch, UTC), kind ("f" file, "d" empty folder → "key/" marker),
// source index.
//
// Symbolic links to folders are not followed (no cycles; reported as
// skipped); links to files upload their target. Pure function of the file
// system; runs on a worker thread.
class LocalScanner {
public:
    struct Result {
        qint64 files = 0;
        qint64 folders = 0;          // empty folders (markers)
        qint64 bytes = 0;
        qint64 skippedCount = 0;
        QStringList skipped;         // "path: reason", the first kMaxListed
        QString error;               // the plan could not be written
        bool cancelled = false;
    };
    static constexpr int kMaxListed = 50;
    static constexpr int kMaxKeyBytes = 1024;   // S3's limit, UTF-8

    // sources: absolute paths of files and folders; each lands at
    // prefix + its name. found: files seen so far (for progress).
    static Result scan(const QStringList& sources, const QString& prefix, const QString& planPath,
                       const std::atomic<bool>& cancel, std::atomic<qint64>& found);

    // Sort key of a directory entry in S3 key order.
    static QByteArray sortKey(const QString& name, bool folder);
};

} // namespace qbucket
