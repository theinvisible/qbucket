#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;

namespace qbucket {

class AccountStore;
class BookmarkModel;
class LocationStore;

// "Manage Bookmarks": rename, reorder and remove bookmarks. Every change
// goes straight to the LocationStore (nothing on a server changes).
class ManageBookmarksDialog : public QDialog {
    Q_OBJECT

public:
    ManageBookmarksDialog(LocationStore* store, AccountStore* accounts, QWidget* parent = nullptr);

    [[nodiscard]] QListView* view() const { return m_view; }
    [[nodiscard]] QLineEdit* nameEdit() const { return m_name; }
    [[nodiscard]] QPushButton* upButton() const { return m_up; }
    [[nodiscard]] QPushButton* downButton() const { return m_down; }
    [[nodiscard]] QPushButton* removeButton() const { return m_remove; }

private:
    void updateState_();
    [[nodiscard]] int currentRow_() const;
    void selectRow_(int row);
    void move_(int delta);

    LocationStore* m_store;
    BookmarkModel* m_model;
    QListView* m_view = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_place = nullptr;
    QPushButton* m_up = nullptr;
    QPushButton* m_down = nullptr;
    QPushButton* m_remove = nullptr;
};

} // namespace qbucket
