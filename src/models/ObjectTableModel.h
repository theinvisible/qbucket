#pragma once

#include <qbucket/s3/Error.h>
#include <qbucket/s3/ListObjectsPager.h>

#include <QAbstractTableModel>
#include <QCollator>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <memory>
#include <vector>

namespace qbucket {

class AccountSessions;

// The object list of one location (account, bucket, prefix), PLAN.md §13.1:
// pages of 1000 keys through fetchMore, never the whole bucket; compact
// rows, display strings made in data(); one insert per page (two with
// folders first). Folder mode lists with "/" as delimiter, flat mode
// without.
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
    enum Column { NameColumn, SizeColumn, ModifiedColumn, StorageClassColumn, ColumnCount };
    enum Role {
        KeyRole = Qt::UserRole + 1,   // QString, the full key (folders with the trailing "/")
        IsFolderRole,                 // bool: a common prefix
        SizeRole,                     // qint64 (folders: -1)
        ModifiedRole,                 // QDateTime UTC (folders: invalid)
        StorageClassRole,             // QString
        NameRole,                     // QString, relative to the location's prefix
    };
    enum class Mode { Folders, Flat };
    enum class State {
        Idle,       // no location
        Loading,    // first page on its way, no rows yet
        Ready,      // rows shown; more pages may follow (canFetchMore)
        Complete,   // every page loaded
        Error,      // the listing failed — rows so far stay, the listing is incomplete
    };
    Q_ENUM(State)

    // Sorting runs in the background from this many rows on.
    static constexpr int kBackgroundSortRows = 5000;
    static constexpr int kPageSize = 1000;

    explicit ObjectTableModel(AccountSessions* sessions, QObject* parent = nullptr);
    ~ObjectTableModel() override;

    // Lists a location from its start. `prefix` ends with "/" or is empty.
    void setLocation(const QUuid& account, const QString& bucket, const QString& prefix,
                     Mode mode = Mode::Folders);
    void clear();
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
    // True while a sort is due or running in the background.
    [[nodiscard]] bool isSorting() const { return m_sortRunning || m_sortDebounce.isActive(); }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    // column -1: back to server order.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    [[nodiscard]] int sortColumn() const { return m_sortColumn; }
    [[nodiscard]] Qt::SortOrder sortOrder() const { return m_sortOrder; }

signals:
    void stateChanged(qbucket::ObjectTableModel::State state);
    // After every page (counts, bytes).
    void pageLoaded();
    void sortFinished();

private:
    enum Flag : quint8 { Folder = 1 };
    struct Row {
        QString key;
        qint64 size = 0;
        qint64 mtimeMs = 0;          // UTC, ms since epoch
        quint16 storageClass = 0;    // index into m_storageClasses; 0: none
        quint8 flags = 0;
    };
    struct SortSpec {
        int column = -1;
        Qt::SortOrder order = Qt::AscendingOrder;
    };

    void start_();
    void fetchPage_();
    void onPage_(const s3::Result<s3::ListPage>& page);
    void appendRows_(std::vector<Row>&& folders, std::vector<Row>&& objects);
    void setState_(State state);
    void resetListing_();
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
    std::unique_ptr<QObject> m_run;   // context of the current listing's callbacks
    int m_pages = 0;

    std::vector<Row> m_rows;        // arrival order
    std::vector<quint32> m_order;   // all rows, display order
    std::vector<quint32> m_view;    // m_order through the filter
    qsizetype m_folderRows = 0;     // in m_rows
    qsizetype m_orderFolders = 0;   // leading folders in m_order (server order only)
    qsizetype m_viewFolders = 0;    // leading folders in m_view (server order only)
    qint64 m_bytes = 0;
    QStringList m_storageClasses{QString()};

    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    quint64 m_orderVersion = 0;     // changes with every row set or sort change
    bool m_sortRunning = false;
    bool m_sortPending = false;
    QTimer m_sortDebounce;
    QCollator m_collator;
};

} // namespace qbucket
