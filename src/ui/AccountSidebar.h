#pragma once

#include <QSet>
#include <QUuid>
#include <QWidget>

class QLabel;
class QPushButton;
class QTreeView;

namespace qbucket {

class AccountSessions;
class AccountStore;
class AccountTreeModel;
class SecretStore;

// The left column of the main window: "ACCOUNTS", the account → bucket
// tree, "Add Account…". Owns the account context menu (edit, external
// buckets, remove) and asks for a secret the keychain doesn't have.
// Without stores (tests) it shows the empty state, disabled.
class AccountSidebar : public QWidget {
    Q_OBJECT

public:
    AccountSidebar(AccountStore* accounts, SecretStore* secrets, AccountSessions* sessions,
                   QWidget* parent = nullptr);

    [[nodiscard]] QTreeView* view() const { return m_tree; }
    [[nodiscard]] AccountTreeModel* model() const { return m_model; }

    // Lists the current account's buckets again.
    void refreshCurrent();
    void refreshIcons();

signals:
    void addAccountRequested();
    // The current row became a bucket / an account (or a status row of it).
    void bucketSelected(const QUuid& account, const QString& bucket);
    void accountSelected(const QUuid& account);
    void statusMessage(const QString& text, int timeoutMs);

private:
    void showContextMenu_(const QPoint& pos);
    void onCurrentChanged_(const QModelIndex& current);
    void onActivated_(const QModelIndex& index);
    void editAccount_(const QUuid& id);
    void removeAccount_(const QUuid& id);
    void addExternalBucket_(const QUuid& id);
    void removeExternalBucket_(const QUuid& id, const QString& bucket);
    void askForSecret_(const QUuid& id);
    void updateEmptyState_();

    AccountStore* m_accounts;
    SecretStore* m_secrets;
    AccountSessions* m_sessions;
    AccountTreeModel* m_model = nullptr;
    QTreeView* m_tree = nullptr;
    QLabel* m_empty = nullptr;
    QPushButton* m_addButton = nullptr;
    QSet<QUuid> m_askingSecret;   // one prompt per account at a time
};

} // namespace qbucket
