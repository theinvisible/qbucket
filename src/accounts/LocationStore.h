#pragma once

#include "domain/Bookmark.h"
#include "domain/Location.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QUuid>

#include <optional>

class QTimer;

namespace qbucket {

// Bookmarks (in the user's order) and the recently visited locations, in
// locations.json under AppConfigLocation. Only places, never anything
// secret.
//
// Bookmark changes are written at once; visits are debounced (300 ms) since
// every navigation records one — flush() writes what is pending (quit).
// Writes are synchronous disk I/O of a small file (QSaveFile).
// Thread affinity: GUI thread.
class LocationStore : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxRecent = 20;

    explicit LocationStore(QObject* parent = nullptr);
    // Explicit file, for tests.
    explicit LocationStore(const QString& filePath, QObject* parent = nullptr);
    ~LocationStore() override;

    [[nodiscard]] QList<Bookmark> bookmarks() const { return m_bookmarks; }
    [[nodiscard]] std::optional<Bookmark> bookmark(const QUuid& id) const;
    // Newest first; only locations with a bucket.
    [[nodiscard]] QList<Location> recent() const { return m_recent; }

    // An empty name becomes the location's last segment (or the bucket).
    Bookmark addBookmark(const QString& name, const Location& location);
    bool renameBookmark(const QUuid& id, const QString& name);
    bool removeBookmark(const QUuid& id);
    // To `index` in the list (clamped).
    bool moveBookmark(const QUuid& id, int index);

    // A location was shown: first in the recent list, once.
    void visited(const Location& location);
    void clearRecent();

    // An account removed / a bucket deleted: its places go too.
    void forgetAccount(const QUuid& account);
    void forgetBucket(const QUuid& account, const QString& bucket);

    void flush();

    // What the location is called when nobody named it.
    [[nodiscard]] static QString defaultName(const Location& location);

    [[nodiscard]] QString fileName() const { return m_path; }
    // Why the file could not be read at start ("" when fine).
    [[nodiscard]] QString loadProblem() const { return m_loadProblem; }

signals:
    void bookmarksChanged();
    void recentChanged();

private:
    void load_();
    void save_();
    void scheduleSave_();

    QString m_path;
    QList<Bookmark> m_bookmarks;
    QList<Location> m_recent;
    QTimer* m_saveTimer = nullptr;
    QString m_loadProblem;
};

} // namespace qbucket
