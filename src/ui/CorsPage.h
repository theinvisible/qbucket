#pragma once

#include "ui/BucketConfigPage.h"

class QPushButton;
class QTableWidget;
class QTabWidget;

namespace qbucket {

class CodeEditor;

// CORS rules: a table (one rule per row; lists separated by commas) and the
// XML. The XML is what is applied; the table writes it. A document with
// parts the table can't show is edited as XML only.
class CorsPage : public BucketConfigPage {
    Q_OBJECT

public:
    enum Column { IdColumn, OriginsColumn, MethodsColumn, HeadersColumn, ExposeColumn, MaxAgeColumn, ColumnCount };

    CorsPage(const Context& context, QWidget* parent = nullptr);

    [[nodiscard]] QTableWidget* table() const { return m_table; }
    [[nodiscard]] CodeEditor* xmlEditor() const { return m_xml; }
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }
    [[nodiscard]] QPushButton* addButton() const { return m_add; }
    [[nodiscard]] QPushButton* removeRuleButton() const { return m_removeRule; }

protected:
    void showDocument_(const s3::BucketConfigDocument& doc) override;
    [[nodiscard]] QByteArray editedDocument_() const override;
    void setEditable_(bool editable) override;

private:
    void fillTable_(const s3::CorsConfiguration& config);
    void tableToXml_();
    void xmlToTable_();

    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_table = nullptr;
    CodeEditor* m_xml = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_removeRule = nullptr;
    bool m_syncing = false;
    bool m_tableUsable = true;
    bool m_editable = false;
};

} // namespace qbucket
