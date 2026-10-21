#pragma once

#include <QDialog>
#include <QUuid>

class QLabel;
class QPushButton;
class QTableWidget;

namespace qbucket {

class AccountStore;
class OpenFileService;

// "Open Files": the objects opened in another application, each with its
// state — open, changed (not uploaded), uploading — and what can be done:
// upload the changes, ignore them, open again, show the folder, close it
// (deletes the local copy). Uploads go through the gate (ReplaceObjectJob).
class OpenFilesDialog : public QDialog {
    Q_OBJECT

public:
    OpenFilesDialog(OpenFileService* files, AccountStore* accounts, QWidget* parent = nullptr);

    [[nodiscard]] QTableWidget* table() const { return m_table; }
    [[nodiscard]] QPushButton* uploadButton() const { return m_upload; }
    [[nodiscard]] QPushButton* discardButton() const { return m_discard; }

signals:
    void showFolderRequested(const QString& path);

private:
    void reload_();
    void updateButtons_();
    [[nodiscard]] QUuid current_() const;

    OpenFileService* m_files;
    AccountStore* m_accounts;
    QTableWidget* m_table = nullptr;
    QLabel* m_empty = nullptr;
    QPushButton* m_upload = nullptr;
    QPushButton* m_ignore = nullptr;
    QPushButton* m_again = nullptr;
    QPushButton* m_folder = nullptr;
    QPushButton* m_discard = nullptr;
};

} // namespace qbucket
