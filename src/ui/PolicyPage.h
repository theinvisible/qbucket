#pragma once

#include "ui/BucketConfigPage.h"

class QComboBox;
class QLabel;

namespace qbucket {

class CodeEditor;

// The bucket policy (JSON) with live validation, what it grants publicly,
// and templates: public read, deny insecure transport, restrict to an IP
// range. A template replaces the editor's text (Revert brings it back).
class PolicyPage : public BucketConfigPage {
    Q_OBJECT

public:
    PolicyPage(const Context& context, QWidget* parent = nullptr);

    [[nodiscard]] CodeEditor* editor() const { return m_editor; }
    [[nodiscard]] QComboBox* templates() const { return m_templates; }
    [[nodiscard]] QLabel* findings() const { return m_findings; }
    // A template's text for this bucket (index into templates(), from 1).
    [[nodiscard]] QString templateText(int index) const;

protected:
    void showDocument_(const s3::BucketConfigDocument& doc) override;
    [[nodiscard]] QByteArray editedDocument_() const override;
    void setEditable_(bool editable) override;

private:
    void updateFindings_();

    CodeEditor* m_editor = nullptr;
    QComboBox* m_templates = nullptr;
    QLabel* m_findings = nullptr;
};

} // namespace qbucket
