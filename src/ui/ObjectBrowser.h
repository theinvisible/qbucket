#pragma once

#include "domain/Location.h"
#include "models/ObjectSelection.h"
#include "models/ObjectTableModel.h"

#include <QWidget>

class QAction;
class QMimeData;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableView;

#include <optional>

namespace qbucket {

class ObjectRowDelegate;

class AccountSessions;
class AccountStore;
class BrowseController;
class PathBar;

// The middle column: path bar, the object list of the current location
// and what it says when there is nothing to list — loading, an empty
// folder, an error with "Try Again" (never an empty list for an error).
// A banner above the list marks a stale (cached) or incomplete listing.
// Follows BrowseController; opening a folder navigates into it.
//
// Without sessions (tests without accounts) it shows a hint only.
class ObjectBrowser : public QWidget {
    Q_OBJECT

public:
    ObjectBrowser(AccountStore* accounts, AccountSessions* sessions, QWidget* parent = nullptr);

    [[nodiscard]] BrowseController* navigation() const { return m_nav; }
    [[nodiscard]] ObjectTableModel* model() const { return m_model; }
    [[nodiscard]] QTableView* view() const { return m_view; }
    [[nodiscard]] PathBar* pathBar() const { return m_pathBar; }

    void openBucket(const QUuid& account, const QString& bucket);
    void setMode(ObjectTableModel::Mode mode);
    [[nodiscard]] ObjectTableModel::Mode mode() const { return m_mode; }
    void refresh();
    void setFilterText(const QString& text);
    void searchPrefix(const QString& text);
    void editPath();

    // Actions on the list's selection, for the Edit menu and the context
    // menu; their shortcuts work while the list has the focus.
    [[nodiscard]] QAction* openAction() const { return m_actOpen; }
    [[nodiscard]] QAction* copyAction() const { return m_actCopy; }
    [[nodiscard]] QAction* cutAction() const { return m_actCut; }
    [[nodiscard]] QAction* pasteAction() const { return m_actPaste; }
    [[nodiscard]] QAction* copyKeyAction() const { return m_actCopyKey; }
    [[nodiscard]] QAction* copyUriAction() const { return m_actCopyUri; }
    [[nodiscard]] QAction* renameAction() const { return m_actRename; }
    [[nodiscard]] QAction* deleteAction() const { return m_actDelete; }
    [[nodiscard]] QAction* downloadAction() const { return m_actDownload; }
    [[nodiscard]] QAction* downloadToAction() const { return m_actDownloadTo; }

    // The gate's verdict for the mutating actions (MainWindow decides).
    void setMutationsAllowed(bool allowed, const QString& reason);
    // Downloads only read: also on read-only accounts.
    void setDownloadsAllowed(bool allowed, const QString& reason);
    // Local files may be dropped here (and onto folder rows).
    void setUploadsAllowed(bool allowed);
    // The local files of a drag, or empty when it isn't one of only local
    // files and folders.
    [[nodiscard]] static QStringList localPaths(const QMimeData* mime);

    // Selected rows (column 0), in view order.
    [[nodiscard]] QModelIndexList selectedRows() const;
    // The selection as objects (keys only) for a copy, a cut or a drag.
    [[nodiscard]] ObjectSelection selection(ObjectSelection::Intent intent) const;

    // "1 204 objects · 38.2 GB loaded · 3 selected" for the status bar.
    [[nodiscard]] QString statusText() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // Counts, state or selection changed.
    void statusChanged();
    void locationChanged(const qbucket::Location& location);
    // Local files and folders dropped onto the list (prefix: the folder row
    // they were dropped on, or the current one).
    void filesDropped(const QStringList& paths, const QString& prefix);
    // Objects of this account dropped onto the list (move: with Shift).
    void objectsDropped(const qbucket::ObjectSelection& selection, const QString& prefix, bool move);
    // Paste: the clipboard's objects into the current folder (a cut moves).
    void pasteRequested(const qbucket::ObjectSelection& selection, const QString& prefix);

private:
    void onLocationChanged_(const Location& location);
    void onAccountsChanged_();
    void updatePage_();
    void onActivated_(const QModelIndex& index);
    void maybePrefetch_();
    void rememberCurrent_();
    void restoreCurrent_();
    void showMessage_(const QString& text, bool retry);
    void buildActions_();
    void updateActions_();
    void showContextMenu_(const QPoint& pos);
    void copySelection_(bool asUri);
    void copyObjects_(ObjectSelection::Intent intent);
    void paste_();
    // The clipboard's objects, if they can go into the current location;
    // otherwise *why (empty: nothing to paste).
    [[nodiscard]] std::optional<ObjectSelection> pasteable_(QString* why) const;
    // A drag of objects may land in `prefix` of the current bucket.
    [[nodiscard]] bool acceptsObjects_(const ObjectSelection& sel, const QString& prefix) const;
    // Drag & drop in: the prefix the files would land in; -1 / "" if none.
    [[nodiscard]] QString dropPrefixAt_(const QPoint& pos, int* row) const;
    void setDropRow_(int row);
    bool handleDrop_(QEvent* event);

    AccountStore* m_accounts;
    AccountSessions* m_sessions;
    BrowseController* m_nav = nullptr;
    ObjectTableModel* m_model = nullptr;
    ObjectTableModel::Mode m_mode = ObjectTableModel::Mode::Folders;

    PathBar* m_pathBar = nullptr;
    QLabel* m_banner = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_messagePage = nullptr;
    QLabel* m_message = nullptr;
    QPushButton* m_retry = nullptr;
    QTableView* m_view = nullptr;
    QString m_currentKey;   // kept across model resets

    QAction* m_actOpen = nullptr;
    QAction* m_actCopy = nullptr;
    QAction* m_actCut = nullptr;
    QAction* m_actPaste = nullptr;
    QAction* m_actCopyKey = nullptr;
    QAction* m_actCopyUri = nullptr;
    QAction* m_actRename = nullptr;
    QAction* m_actDelete = nullptr;
    QAction* m_actDownload = nullptr;
    QAction* m_actDownloadTo = nullptr;
    bool m_downloadsAllowed = false;
    bool m_uploadsAllowed = false;
    ObjectRowDelegate* m_delegate = nullptr;
    QString m_downloadsReason;
    bool m_mutationsAllowed = false;
    QString m_mutationsReason;
};

} // namespace qbucket
