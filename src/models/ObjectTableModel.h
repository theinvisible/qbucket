#pragma once

#include <qbucket/s3/Error.h>
#include <qbucket/s3/ListObjectVersionsPager.h>
#include <qbucket/s3/ListObjectsPager.h>

#include <QAbstractTableModel>
#include <QCollator>
#include <QDateTime>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <memory>
#include <vector>

namespace qbucket {

class AccountSessions;

// The object list of one location (account, bucket, prefix), PLAN.md §13.1:
// pages of 1000 keys on request (loadMore), never the whole bucket; compact
// rows, display strings made in data(); one insert per page (two with
// folders first). Folder mode lists with "/" as delimiter, flat mode
// without. Versions mode lists every version and delete marker
// (ListObjectVersions, with "/"): a key's versions follow each other, newest
// first; it is never served from the cache.
//
// Listings of recently visited locations are kept (up to kCachePages
// pages each) and shown at once when the location is opened again —
// marked stale while the same pages are listed afresh in the background,
// then replaced in one go (PLAN.md §12.5: a cached view is never passed off
// as current). reload() always lists afresh.
//
// Sorting and the live filter are the model's own (no proxy): with
// 100k rows a synchronous proxy sort would stall the GUI. Rows are shown
// in server order, folders first, until a column is sorted; large sorts run
// on the global thread pool and are applied with layoutChanged, so the
// selection survives. Pages that arrive while sorted are appended, then
// sorted in.
//
// Thread affinity: GUI thread.
class ObjectTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    // Persisted as column indices: append only.
    enum Column { NameColumn, SizeColumn, ModifiedColumn, StorageClassColumn, VersionColumn, ColumnCount };
    enum Role {
        KeyRole = Qt::UserRole + 1,   // QString, the full key (folders with the trailing "/")
        IsFolderRole,                 // bool: a common prefix
        SizeRole,                     // qint64 (folders: -1)
        ModifiedRole,                 // QDateTime UTC (folders: invalid)
        StorageClassRole,             // QString
        NameRole,                     // QString, relative to the location's prefix
        VersionIdRole,                // QString (Versions mode; else empty)
        IsLatestRole,                 // bool: the key's current entry (always true outside Versions mode)
        IsDeleteMarkerRole,           // bool
    };
    enum class Mode { Folders, Flat, Versions };
    enum class State {
        Idle,       // no location
        Loading,    // first page on its way, no rows yet
        Ready,      // rows shown; more pages may follow (canLoadMore)
        Complete,   // every page loaded
        Error,      // the listing failed — rows so far stay, the listing is incomplete
    };
    Q_ENUM(State)

    // Sorting runs in the background from this many rows on.
    static constexpr int kBackgroundSortRows = 5000;
    static constexpr int kPageSize = 1000;
    static constexpr int kCacheEntries = 12;
    static constexpr int kCachePages = 20;
    static constexpr qsizetype kCacheRows = 300'000;   // over all entries

    explicit ObjectTableModel(AccountSessions* sessions, QObject* parent = nullptr);
    ~ObjectTableModel() override;

    // Lists a location from its start — or shows its cached listing, stale,
    // while listing it again. `prefix` ends with "/" or is empty.
    void setLocation(const QUuid& account, const QString& bucket, const QString& prefix,
                     Mode mode = Mode::Folders);
    void clear();
    // Lists the location afresh, never from the cache.
    void reload();
    // Server-side search: lists prefix + text (names starting with it);
    // empty: the whole location again.
    void setSearchPrefix(const QString& text);
    // Live filter on the loaded rows: names containing text, any case.
    void setFilterText(const QString& text);

    [[nodiscard]] QUuid account() const { return m_account; }
    [[nodiscard]] QString bucket() const { return m_bucket; }
    [[nodiscard]] QString prefix() const { return m_prefix; }
    [[nodiscard]] Mode mode() const { return m_mode; }
    [[nodiscard]] QString searchPrefix() const { return m_search; }
    [[nodiscard]] QString filterText() const { return m_filter; }
    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] const s3::Error& error() const { return m_error; }
    [[nodiscard]] QString errorText() const { return m_errorText; }

    // Over all loaded rows, filter or not.
    [[nodiscard]] qsizetype loadedObjects() const { return qsizetype(m_rows.size()) - m_folderRows; }
    [[nodiscard]] qsizetype loadedFolders() const { return m_folderRows; }
    [[nodiscard]] qint64 loadedBytes() const { return m_bytes; }
    [[nodiscard]] int pagesLoaded() const { return m_pages; }
    // Showing a cached listing that is being refreshed.
    [[nodiscard]] bool isStale() const { return m_stale; }
    [[nodiscard]] QDateTime staleSince() const { return m_staleSince; }
    // True while a sort is due or running in the background.
    [[nodiscard]] bool isSorting() const { return m_sortRunning || m_sortDebounce.isActive(); }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    // Rows can be dragged (ObjectSelection: keys only, nothing loaded).
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    // column -1: back to server order.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // Paging is the caller's decision (the browser asks near the end of the
    // list). Deliberately not canFetchMore/fetchMore: QTreeView calls those
    // from every layout pass and would page through the whole bucket.
    [[nodiscard]] bool canLoadMore() const;
    // Requests the next page; its rows arrive from the event loop.
    void loadMore();

    // Row of a key in the current view (its newest entry); -1 if not shown.
    [[nodiscard]] int rowOfKey(const QString& key) const;
    // Row of one version in the current view; -1 if not shown.
    [[nodiscard]] int rowOfVersion(const QString& key, const QString& versionId) const;

    [[nodiscard]] int sortColumn() const { return m_sortColumn; }
    [[nodiscard]] Qt::SortOrder sortOrder() const { return m_sortOrder; }

signals:
    void stateChanged(qbucket::ObjectTableModel::State state);
    // After every page (counts, bytes).
    void pageLoaded();
    void sortFinished();
    void staleChanged(bool stale);

private:
    enum Flag : quint8 { Folder = 1, Older = 2, DeleteMarker = 4 };
    struct Row {
        QString key;
        qint64 size = 0;
        qint64 mtimeMs = 0;          // UTC, ms since epoch
        quint16 storageClass = 0;    // index into m_storageClasses; 0: none
        quint8 flags = 0;
        quint32 version = 0;         // index into m_versionIds; 0: none (fits the padding)
    };
    struct SortSpec {
        int column = -1;
        Qt::SortOrder order = Qt::AscendingOrder;
    };

    struct Snapshot {
        QUuid account;
        QString bucket;
        QString prefix;
        Mode mode = Mode::Folders;
        std::vector<Row> rows;
        std::vector<size_t> pageEnds;
        QStringList storageClasses;
        bool complete = false;
        QDateTime at;
    };

    void start_(bool useCache);
    void fetchPage_();
    void onPage_(const s3::Result<s3::ListPage>& page);
    void onVersionPage_(const s3::Result<s3::ListObjectVersionsPage>& page);
    void pageArrived_(std::vector<Row>&& folders, std::vector<Row>&& objects);
    void listingFailed_(const s3::Error& error);
    void parsePage_(const s3::ListPage& page, std::vector<Row>& folders, std::vector<Row>& objects);
    void parseVersionPage_(const s3::ListObjectVersionsPage& page, std::vector<Row>& folders, std::vector<Row>& objects);
    [[nodiscard]] bool hasPager_() const { return m_pager || m_versionPager; }
    [[nodiscard]] bool pagerHasMore_() const;
    [[nodiscard]] bool pagerFetching_() const;
    void appendRows_(std::vector<Row>&& folders, std::vector<Row>&& objects);
    void setState_(State state);
    void resetListing_();
    void keepForCache_();
    [[nodiscard]] bool restoreSnapshot_();
    void swapInFresh_();
    void replaceRows_(std::vector<Row>&& rows, std::vector<size_t>&& pageEnds);
    void setStale_(bool stale);
    void scheduleSort_();
    void runSort_();
    void applyOrder_(std::vector<quint32> order, quint64 version);
    void rebuildView_();
    [[nodiscard]] bool passesFilter_(const Row& row) const;
    [[nodiscard]] QString nameOf_(const Row& row) const;
    [[nodiscard]] quint16 internStorageClass_(const QString& name);
    [[nodiscard]] const Row& rowAt_(int viewRow) const { return m_rows[m_view[size_t(viewRow)]]; }

    AccountSessions* m_sessions;

    QUuid m_account;
    QString m_bucket;
    QString m_prefix;
    QString m_search;
    QString m_filter;
    Mode m_mode = Mode::Folders;

    State m_state = State::Idle;
    s3::Error m_error;
    QString m_errorText;
    std::unique_ptr<s3::ListObjectsPager> m_pager;
    std::unique_ptr<s3::ListObjectVersionsPager> m_versionPager;   // Versions mode
    std::unique_ptr<QObject> m_run;   // context of the current listing's callbacks
    int m_pages = 0;

    std::vector<Row> m_rows;        // arrival order
    std::vector<size_t> m_pageEnds; // m_rows.size() after each page
    std::vector<quint32> m_order;   // all rows, display order
    std::vector<quint32> m_view;    // m_order through the filter
    qsizetype m_folderRows = 0;     // in m_rows
    qsizetype m_orderFolders = 0;   // leading folders in m_order (server order only)
    qsizetype m_viewFolders = 0;    // leading folders in m_view (server order only)
    qint64 m_bytes = 0;
    QStringList m_storageClasses{QString()};
    std::vector<QString> m_versionIds{QString()};   // Versions mode; [0] unused

    std::vector<std::unique_ptr<Snapshot>> m_cache;   // most recent first
    bool m_stale = false;
    QDateTime m_staleSince;
    int m_stalePages = 0;            // pages of the cached listing
    std::vector<Row> m_fresh;        // the refresh of a stale listing, until complete
    std::vector<size_t> m_freshEnds;
    int m_freshPages = 0;

    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    quint64 m_orderVersion = 0;     // changes with every row set or sort change
    bool m_sortRunning = false;
    bool m_sortPending = false;
    QTimer m_sortDebounce;
    QCollator m_collator;
};

} // namespace qbucket
