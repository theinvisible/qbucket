#pragma once

#include <QDateTime>
#include <QDialog>

class QCheckBox;
class QDateTimeEdit;

namespace qbucket {

// "Restore Deleted Objects": where, and optionally only what was deleted
// after a moment (an accident after a planned clean-up). The job's preview
// shows what comes back before anything happens.
class UndeleteDialog : public QDialog {
    Q_OBJECT

public:
    explicit UndeleteDialog(const QString& scope, QWidget* parent = nullptr);

    // Invalid: everything deleted, whenever.
    [[nodiscard]] QDateTime deletedAfter() const;
    [[nodiscard]] QCheckBox* onlyAfterBox() const { return m_onlyAfter; }
    [[nodiscard]] QDateTimeEdit* afterEdit() const { return m_after; }

private:
    QCheckBox* m_onlyAfter = nullptr;
    QDateTimeEdit* m_after = nullptr;
};

} // namespace qbucket
