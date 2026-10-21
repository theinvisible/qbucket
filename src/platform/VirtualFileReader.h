#pragma once

#include "controllers/DragStaging.h"
#include "platform/VirtualFileSource.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace qbucket {

namespace s3 {
class ListObjectsPager;
class Session;
}

// A VirtualFileSource over an account's Session: the selection is listed
// (folders completely, objects by HEAD) and mapped onto Windows-safe names
// (LocalPathMapper); bytes come as ranged GETs into memory with If-Match on
// the ETag the listing saw — a change while it is copied is an error, never
// a mix of two versions. Sequential reads fetch kAhead chunks ahead; at the
// end the MD5 of what was handed out is compared with a single-part ETag
// (not under KMS / SSE-C) before the last bytes go out. Everything else is
// size-checked only, and summary() says which.
//
// Its own worker thread (with an event loop) runs the requests; callers
// block on a condition variable and must not be the GUI thread.
class VirtualFileReader final : public VirtualFileSource {
public:
    static constexpr qint64 kChunk = qint64(8) * 1024 * 1024;
    static constexpr int kAhead = 2;
    static constexpr int kMaxEntries = 10000;

    VirtualFileReader(s3::Session* session, DragStaging::Selection selection);
    ~VirtualFileReader() override;
    VirtualFileReader(const VirtualFileReader&) = delete;
    VirtualFileReader& operator=(const VirtualFileReader&) = delete;

    Status files(std::vector<File>* out) override;
    Status read(std::size_t index, std::int64_t offset, void* buffer, std::uint32_t length,
                std::uint32_t* read) override;
    void cancel() override;
    [[nodiscard]] std::u16string lastError() const override;

    // What happened to each file read to its end, one line each
    // ("key: md5 verified" / "key: size verified" / "key: failed — why").
    [[nodiscard]] QStringList summary() const;
    void setChunkSizeForTesting(qint64 bytes) { m_chunk = bytes; }

private:
    struct Entry {
        QString key;
        QString path;            // mapped, "/"-separated
        bool folder = false;
        qint64 size = 0;
        QString etag;
        QDateTime modified;
        bool headed = false;     // HEAD seen (encryption known)
        QString sse;             // x-amz-server-side-encryption / SSE-C algorithm
    };
    struct Chunk {
        bool done = false;
        QByteArray data;
        QString error;
    };
    struct FileState {
        bool started = false;
        bool headDone = false;
        QString headError;
        bool verify = false;
        std::unique_ptr<QCryptographicHash> md5;
        qint64 hashed = 0;
        std::map<qint64, Chunk> chunks;
        bool finished = false;
    };

    void list_();                                   // worker thread
    void listNext_(int item);                       // worker thread
    void fetchPage_(int item, const QString& parent);   // worker thread
    void listed_(const QString& error);             // worker thread
    void head_(std::size_t index);                  // worker thread
    void fetch_(std::size_t index, qint64 chunk);   // worker thread
    Status fail_(const QString& why);               // with the lock held
    void note_(const QString& line);                // with the lock held
    [[nodiscard]] bool onGuiThread_() const;

    QPointer<s3::Session> m_session;
    DragStaging::Selection m_selection;
    qint64 m_chunk = kChunk;
    QThread m_thread;
    QObject* m_ctx = nullptr;                        // lives on m_thread
    std::unique_ptr<s3::ListObjectsPager> m_pager;   // worker thread only

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    enum class ListState { NotStarted, Listing, Done, Failed } m_listState = ListState::NotStarted;
    std::vector<Entry> m_entries;                   // final order
    std::vector<Entry> m_found;                     // while listing
    std::map<std::size_t, FileState> m_files;
    bool m_cancelled = false;
    QString m_error;
    QStringList m_summary;
};

} // namespace qbucket
