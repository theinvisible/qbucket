#pragma once

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace qbucket {

class Job;

// The confirmation of a planned mutation (PLAN.md §12.2 step 2): account
// and environment, the exact scope from the fresh plan, sample keys,
// warnings, whether it can be undone. Production accounts need a second,
// explicit tick; large scopes need their name typed. Where object lock
// allows it, the governance bypass is offered for this one run — off by
// default, with its own warning and "BYPASS <name>" to type. Cancel is the
// default button. Accepting confirms the job, anything else cancels it.
//
// Window-modal, opened with open(): it waits for the user, never on I/O.
class OperationPreviewDialog : public QDialog {
    Q_OBJECT

public:
    explicit OperationPreviewDialog(Job* job, QWidget* parent = nullptr);

    [[nodiscard]] QPushButton* confirmButton() const { return m_confirm; }
    [[nodiscard]] QCheckBox* productionCheck() const { return m_production; }
    [[nodiscard]] QLineEdit* typedConfirmation() const { return m_typed; }
    // Transfers with existing targets: what to do with them.
    [[nodiscard]] QComboBox* conflictPolicy() const { return m_policy; }
    [[nodiscard]] QCheckBox* bypassCheck() const { return m_bypass; }
    // What has to be typed right now (empty: nothing).
    [[nodiscard]] QString expectedTyping() const;

private:
    void updateButton_();

    QPointer<Job> m_job;
    QPushButton* m_confirm = nullptr;
    QCheckBox* m_production = nullptr;
    QLineEdit* m_typed = nullptr;
    QComboBox* m_policy = nullptr;
    QCheckBox* m_bypass = nullptr;
    QLabel* m_bypassWarning = nullptr;
    QLabel* m_typeLabel = nullptr;
    bool m_answered = false;
};

} // namespace qbucket
