#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUuid>

class QTimer;

namespace qbucket {

class Job;
class OperationController;

// Local copies of objects for dragging them out into a file manager
// (PLAN.md §6 M7, the Linux / Flatpak way): a drop target needs files, a
// drag can't wait for a download, so the selection is "staged" — downloaded
// into its own folder under the cache with a transient DownloadJob — and a
// later drag of the same selection carries those files.
//
// During a drag only small selections of objects are staged on their own
// (prepare(quiet = true)): known sizes, at most kQuietFiles objects, up to
// the confirmation limit. Everything else is staged on request, the
// DownloadJob's preview asking above the limit.
//
// The cache folder (CacheLocation/drag) is on the host's file system in a
// Flatpak too (~/.var/app/<id>/cache), so a host file manager can read it.
// Staged copies expire after maxAge (the drop target has copied them by
// then); a new session removes what is left.
//
// Thread affinity: GUI thread.
class DragStaging : public QObject {
    Q_OBJECT

public:
    static constexpr int kQuietFiles = 500;

    struct Item {
        QString key;              // object key, or folder prefix with "/"
        bool folder = false;
        qint64 size = -1;         // objects: from the listing; -1 unknown
    };
    struct Selection {
        QUuid account;
        QString bucket;
        QList<Item> items;
    };
    enum class State { None, Preparing, Ready, Failed };
    Q_ENUM(State)

    explicit DragStaging(OperationController* operations, QString root = {}, QObject* parent = nullptr);
    ~DragStaging() override;

    // Above this many bytes staging asks (the DownloadJob's preview); a quiet
    // prepare() never stages more.
    void setConfirmAboveBytes(qint64 bytes) { m_confirmAbove = bytes; }
    void setMaxAgeSecs(int secs) { m_maxAgeSecs = secs; }

    [[nodiscard]] State state(const Selection& selection) const;
    // The staged top-level files / folders of the selection — only when all
    // of them still exist and are young enough; empty otherwise.
    [[nodiscard]] QStringList readyPaths(const Selection& selection) const;
    // Whether a quiet prepare() would stage it.
    [[nodiscard]] bool stagesQuietly(const Selection& selection) const;

    // Starts staging (once per selection). Quiet: only what stagesQuietly().
    void prepare(const Selection& selection, bool quiet);

    // Removes expired copies (and every stray folder of earlier sessions).
    void cleanup();
    [[nodiscard]] QString root() const { return m_root; }

    [[nodiscard]] static QString signature(const Selection& selection);

    // A drop asked for the files before they were there (the drag ran
    // while staging): says so instead of the drop silently doing nothing.
    void reportNotReady(const Selection& selection);

signals:
    void stateChanged(const QString& signature);
    void message(const QString& text);

private:
    struct Stage {
        QUuid id;
        Selection selection;
        State state = State::None;
        QString dir;
        QStringList paths;        // top-level, absolute
        QDateTime readyAt;
    };
    void finished_(const QString& sig, Job* job);
    void remove_(const QString& sig);

    OperationController* m_ops;
    QString m_root;
    qint64 m_confirmAbove = qint64(100) * 1024 * 1024;
    int m_maxAgeSecs = 3600;
    QHash<QString, Stage> m_stages;
    QTimer* m_sweep;
};

} // namespace qbucket
