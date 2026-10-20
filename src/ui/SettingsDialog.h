#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QStackedWidget;

namespace qbucket {

class SettingsController;

// Preferences dialog in the xyz-profiler look: a category sidebar
// (#prefSidebar / #prefCat) next to a stacked page area. Changes apply
// immediately. Appearance (M0) and Transfers (M3); Network / Accounts
// pages join with the milestones that need them.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(SettingsController* settings, QWidget* parent = nullptr);

private:
    QWidget* buildAppearancePage_();
    QWidget* buildTransfersPage_();
    void addCategory_(const QString& label, QWidget* page);

    SettingsController* m_settings;
    QWidget*            m_sidebar   = nullptr;
    QStackedWidget*     m_pages     = nullptr;
    QComboBox*          m_themeCombo = nullptr;
    QLineEdit*          m_downloadDir = nullptr;
    QSpinBox*           m_parallelParts = nullptr;
    QSpinBox*           m_threads = nullptr;
};

} // namespace qbucket
