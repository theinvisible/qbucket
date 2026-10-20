#pragma once

#include "domain/Location.h"
#include "models/ObjectTableModel.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace qbucket {

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
    [[nodiscard]] QTreeView* view() const { return m_view; }
    [[nodiscard]] PathBar* pathBar() const { return m_pathBar; }

    void openBucket(const QUuid& account, const QString& bucket);
    void setMode(ObjectTableModel::Mode mode);
    [[nodiscard]] ObjectTableModel::Mode mode() const { return m_mode; }
    void refresh();
    void setFilterText(const QString& text);
    void searchPrefix(const QString& text);
    void editPath();

    // "1 204 objects · 38.2 GB loaded · 3 selected" for the status bar.
    [[nodiscard]] QString statusText() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // Counts, state or selection changed.
    void statusChanged();
    void locationChanged(const qbucket::Location& location);

private:
    void onLocationChanged_(const Location& location);
    void onAccountsChanged_();
    void updatePage_();
    void onActivated_(const QModelIndex& index);
    void maybePrefetch_();
    void rememberCurrent_();
    void restoreCurrent_();
    void showMessage_(const QString& text, bool retry);

    AccountStore* m_accounts;
    AccountSessions* m_sessions;
    BrowseController* m_nav = nullptr;
    ObjectTableModel* m_model = nullptr;
    ObjectTableModel::Mode m_mode = ObjectTableModel::Mode::Folders;

    PathBar* m_pathBar = nullptr;
    QLabel* m_banner = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_message = nullptr;
    QPushButton* m_retry = nullptr;
    QTreeView* m_view = nullptr;
    QString m_currentKey;   // kept across model resets
};

} // namespace qbucket
