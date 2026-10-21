#pragma once

#include "ui/BucketConfigPage.h"

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace qbucket {

class CodeEditor;

// Lifecycle rules: a list of rules with a form for the selected one (filter
// by prefix and tags; expire current versions after days / on a date;
// remove expired delete markers; one transition by days where the provider
// knows more than one storage class; delete earlier versions; abort
// unfinished uploads) and the XML. The XML is what is applied; the form
// writes it. Rules the form can't show (several transitions, transitions
// by date, size filters, …) are edited as XML only.
class LifecyclePage : public BucketConfigPage {
    Q_OBJECT

public:
    LifecyclePage(const Context& context, QWidget* parent = nullptr);

    [[nodiscard]] QListWidget* rules() const { return m_rules; }
    [[nodiscard]] CodeEditor* xmlEditor() const { return m_xml; }
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }
    [[nodiscard]] QPushButton* addButton() const { return m_add; }
    [[nodiscard]] QLineEdit* prefixEdit() const { return m_prefix; }
    [[nodiscard]] QCheckBox* expireCheck() const { return m_expire; }
    [[nodiscard]] QSpinBox* expireDays() const { return m_expireDays; }
    [[nodiscard]] QCheckBox* noncurrentCheck() const { return m_noncurrent; }
    [[nodiscard]] QSpinBox* noncurrentDays() const { return m_noncurrentDays; }
    [[nodiscard]] QCheckBox* abortCheck() const { return m_abort; }
    [[nodiscard]] QWidget* transitionRow() const { return m_transitionRow; }
    // Whether the form can show (and so edit) these rules.
    [[nodiscard]] static bool formCanShow(const s3::LifecycleConfiguration& config);

protected:
    void showDocument_(const s3::BucketConfigDocument& doc) override;
    [[nodiscard]] QByteArray editedDocument_() const override;
    void setEditable_(bool editable) override;

private:
    QWidget* buildForm_();
    void fillList_();
    void showRule_(int index);
    void formToRule_();
    void configToXml_();
    void xmlToConfig_();
    void updateEnabled_();

    s3::LifecycleConfiguration m_config;
    int m_current = -1;
    bool m_syncing = false;
    bool m_formUsable = true;
    bool m_editable = false;
    QStringList m_classes;

    QTabWidget* m_tabs = nullptr;
    QListWidget* m_rules = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_removeRule = nullptr;
    QWidget* m_form = nullptr;
    QLineEdit* m_id = nullptr;
    QCheckBox* m_enabled = nullptr;
    QLineEdit* m_prefix = nullptr;
    QLineEdit* m_tags = nullptr;
    QCheckBox* m_expire = nullptr;
    QSpinBox* m_expireDays = nullptr;
    QCheckBox* m_expireOn = nullptr;
    QDateEdit* m_expireDate = nullptr;
    QCheckBox* m_markers = nullptr;
    QWidget* m_transitionRow = nullptr;
    QCheckBox* m_transition = nullptr;
    QSpinBox* m_transitionDays = nullptr;
    QComboBox* m_transitionClass = nullptr;
    QCheckBox* m_noncurrent = nullptr;
    QSpinBox* m_noncurrentDays = nullptr;
    QSpinBox* m_keepNewer = nullptr;
    QCheckBox* m_abort = nullptr;
    QSpinBox* m_abortDays = nullptr;
    CodeEditor* m_xml = nullptr;
};

} // namespace qbucket
