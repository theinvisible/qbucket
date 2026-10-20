#pragma once

#include <QDialog>
#include <QUuid>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace qbucket {

class CapabilityController;

// "Server Capabilities" (PLAN.md §3.3): what the account's server and its
// buckets support — state, where it is known from, when and with which
// answer — plus the history of changes and "Check Again". Non-modal; open
// it with open() or show().
class CapabilitiesDialog : public QDialog {
    Q_OBJECT

public:
    CapabilitiesDialog(CapabilityController* capabilities, const QUuid& account, const QString& accountName,
                       const QString& bucket, QWidget* parent = nullptr);

    // "" for the server, else a bucket.
    [[nodiscard]] QString scope() const;
    void setScope(const QString& bucket);

private:
    void fillScopes_(const QString& select);
    void refresh_();
    void loadHistory_();

    CapabilityController* m_caps;
    QUuid m_account;
    QComboBox* m_scope = nullptr;
    QLabel* m_summary = nullptr;
    QTreeWidget* m_table = nullptr;
    QTreeWidget* m_history = nullptr;
    QPushButton* m_recheck = nullptr;
};

} // namespace qbucket
