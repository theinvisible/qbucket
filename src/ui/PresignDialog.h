#pragma once

#include <QDialog>
#include <QStringList>
#include <QUuid>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace qbucket {

class PresignService;

// Presigned links for the selected objects (PLAN.md §6 M4): GET or PUT, an
// expiry up to the provider's limit, "download as file" for GET; the links
// to copy or save as a list (CSV with key, link and expiry). Warnings for
// PUT and long-lived links come from PresignService.
class PresignDialog : public QDialog {
    Q_OBJECT

public:
    PresignDialog(PresignService* service, const QUuid& account, const QString& bucket, const QStringList& keys,
                  int folders, QWidget* parent = nullptr);

    // Links to these versions of the keys (same order): GET only — a PUT
    // link always writes the current version.
    void setVersionIds(const QStringList& versionIds);

    // For tests.
    [[nodiscard]] QComboBox* methodBox() const { return m_method; }
    [[nodiscard]] QSpinBox* amountBox() const { return m_amount; }
    [[nodiscard]] QComboBox* unitBox() const { return m_unit; }
    [[nodiscard]] QCheckBox* downloadBox() const { return m_download; }
    [[nodiscard]] QPushButton* generateButton() const { return m_generate; }
    [[nodiscard]] QPushButton* copyButton() const { return m_copy; }
    [[nodiscard]] QPlainTextEdit* output() const { return m_output; }
    [[nodiscard]] QLabel* warningLabel() const { return m_warnings; }
    [[nodiscard]] int expiresSeconds() const;

signals:
    void linksReady();

private:
    void update_();
    void generate_();
    void save_();

    PresignService* m_service;
    QUuid m_account;
    QString m_bucket;
    QStringList m_keys;
    QStringList m_versionIds;
    QStringList m_csv;   // key,link,expires per line, for Save
    QComboBox* m_method = nullptr;
    QSpinBox* m_amount = nullptr;
    QComboBox* m_unit = nullptr;
    QCheckBox* m_download = nullptr;
    QLabel* m_warnings = nullptr;
    QLabel* m_reason = nullptr;
    QPushButton* m_generate = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_save = nullptr;
};

} // namespace qbucket
