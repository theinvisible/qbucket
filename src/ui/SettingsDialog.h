#pragma once

#include <QDialog>

class QComboBox;
class QStackedWidget;

namespace qbucket {

class SettingsController;

// Preferences dialog in the xyz-profiler look: a category sidebar
// (#prefSidebar / #prefCat) next to a stacked page area. Changes apply
// immediately. M0 has the Appearance page; Transfers / Network / Accounts
// pages join with the milestones that need them.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(SettingsController* settings, QWidget* parent = nullptr);

private:
    QWidget* buildAppearancePage_();
    void addCategory_(const QString& label, QWidget* page);

    SettingsController* m_settings;
    QWidget*            m_sidebar   = nullptr;
    QStackedWidget*     m_pages     = nullptr;
    QComboBox*          m_themeCombo = nullptr;
};

} // namespace qbucket
