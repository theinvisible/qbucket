#pragma once

#include "controllers/EditObjectsJob.h"

#include <qbucket/s3/Types.h>

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace qbucket {

// Headers and user metadata of objects (PLAN.md §6 M4).
//   One object: its current values, edited in place; the result is the
//   difference (EditObjectsJob merges it into a fresh snapshot).
//   Several objects or folders: each field is left alone, set or removed;
//   metadata rows are set, or removed when ticked.
// Values must be printable ASCII (sent as HTTP headers).
class EditMetadataDialog : public QDialog {
    Q_OBJECT

public:
    EditMetadataDialog(const QString& key, const s3::ObjectMeta& current, QWidget* parent = nullptr);
    EditMetadataDialog(int objects, QWidget* parent = nullptr);

    [[nodiscard]] EditObjectsJob::ChangeSpec change() const;
    [[nodiscard]] bool isBulk() const { return m_bulk; }
    // For tests.
    [[nodiscard]] QLineEdit* field(const QString& header) const;
    [[nodiscard]] QComboBox* action(const QString& header) const;
    [[nodiscard]] QTableWidget* metadataTable() const { return m_meta; }
    [[nodiscard]] QPushButton* okButton() const;
    void addMetadataRow(const QString& name, const QString& value, bool remove = false);

private:
    struct Row {
        QString header;
        QLineEdit* edit = nullptr;
        QComboBox* action = nullptr;   // bulk only
        QString s3::ObjectHeaders::*member = nullptr;
    };
    void build_(const QString& intro);
    void validate_();
    [[nodiscard]] s3::ObjectHeaders edited_() const;

    bool m_bulk;
    s3::ObjectHeaders m_before;
    QList<Row> m_rows;
    QTableWidget* m_meta = nullptr;
    QLabel* m_problem = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace qbucket
