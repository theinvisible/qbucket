#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QUuid>

#include <functional>

class QFileSystemWatcher;
class QTimer;

namespace qbucket {

class Job;
class OperationController;

// Objects opened in another application (PLAN.md §6 M7, "open / edit in
// the default app"): each is downloaded into its own folder under the cache
// (a transient DownloadJob), handed to the desktop's default application and
// watched. A change — content, not just a touch — is offered for upload,
// never uploaded on its own; the upload is a ReplaceObjectJob through the
// gate. Versions and read-only accounts open write-protected and are never
// uploaded.
//
// Nothing changed is deleted silently: keepForLater() writes the changed
// files' entries to open-files.json, and the next session lists them again.
// Unchanged copies are removed by cleanup().
//
// Thread affinity: GUI thread; hashing runs on the thread pool.
class OpenFileService : public QObject {
    Q_OBJECT

public:
    enum class State { Downloading, Open, Changed, Uploading, Failed };
    Q_ENUM(State)

    struct Entry {
        QUuid id;
        QUuid account;
        QString bucket;
        QString key;
        QString versionId;        // an earlier version: read-only
        QString path;             // the local copy (absolute)
        QString etag;             // the object's ETag the copy matches
        QByteArray md5;           // the copy's MD5 as downloaded / last uploaded (hex)
        qint64 size = 0;
        QDateTime modified;       // the copy's mtime then
        bool readOnly = false;
        State state = State::Downloading;
        QString error;            // Failed, or why the last upload didn't happen
    };

    // `root`: where the copies go (CacheLocation/open); `store`: the file
    // of kept entries (AppDataLocation/open-files.json). Empty: defaults.
    explicit OpenFileService(OperationController* operations, QString root = {}, QString store = {},
                             QObject* parent = nullptr);
    ~OpenFileService() override;

    // Hands a file to the desktop (QDesktopServices::openUrl in the app;
    // tests record). Without one, files are downloaded but not opened.
    void setOpener(std::function<bool(const QUrl&)> opener) { m_opener = std::move(opener); }
    // Above this size the download asks first (< 0: never).
    void setConfirmAboveBytes(qint64 bytes) { m_confirmAbove = bytes; }
    // For tests: the debounce before a change is looked at.
    void setSettleDelay(int ms) { m_settleMs = ms; }

    // Downloads (once) and opens the object; an entry of the same object
    // that is open already is opened again instead.
    void open(const QUuid& account, const QString& bucket, const QString& key, const QString& versionId = {});
    void upload(const QUuid& id);
    // Takes the file as it is now as the new baseline (no upload).
    void ignoreChange(const QUuid& id);
    // Stops watching and deletes the local copy.
    void discard(const QUuid& id);
    void openAgain(const QUuid& id);

    [[nodiscard]] QList<Entry> entries() const { return m_entries; }
    [[nodiscard]] const Entry* entry(const QUuid& id) const;
    [[nodiscard]] QList<Entry> changedEntries() const;

    // Quitting: the changed entries are written down for the next session;
    // everything unchanged is removed.
    void keepForLater();
    // Removes the copies of unchanged entries and stray folders.
    void cleanup();

    [[nodiscard]] QString root() const { return m_root; }

signals:
    void entriesChanged();
    // A file's content changed in the other application.
    void fileChanged(const QUuid& id);
    void message(const QString& text);

private:
    Entry* find_(const QUuid& id);
    void downloaded_(const QUuid& id, Job* job);
    void watch_(const Entry& e);
    void pathChanged_(const QString& path);
    void settle_(const QUuid& id);
    void hashed_(const QUuid& id, const QByteArray& md5, qint64 size, const QDateTime& modified);
    void uploaded_(const QUuid& id, Job* job, const QByteArray& md5);
    void load_();
    void saveKept_();
    void setState_(Entry* e, State state, const QString& error = {});

    OperationController* m_ops;
    QString m_root;
    QString m_store;
    std::function<bool(const QUrl&)> m_opener;
    qint64 m_confirmAbove = -1;
    int m_settleMs = 1000;
    QList<Entry> m_entries;
    QFileSystemWatcher* m_watcher;
    QHash<QUuid, QTimer*> m_settle;
    QHash<QUuid, QByteArray> m_pendingMd5;   // the content an upload started with
};

} // namespace qbucket
