#pragma once

#include <QMainWindow>

class QAction;
class QActionGroup;
class QButtonGroup;
class QDockWidget;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace qbucket {

class AccountSessions;
class AccountStore;
class SecretStore;
class SettingsController;
class ThemeManager;

// The QBucket main window: toolbar, sidebar (accounts → buckets) | object
// view | detail pane, a bottom transfer dock and the status bar
// (PLAN.md §4). M0 builds the full layout with empty placeholder models;
// the S3 milestones fill the views in.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Without account / secret stores (tests) account management is off.
    MainWindow(SettingsController* settings, ThemeManager* theme,
               AccountStore* accounts = nullptr, SecretStore* secrets = nullptr,
               QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenuBar_();
    void buildToolBar_();
    void buildCentralWidget_();
    void buildTransferDock_();
    void buildStatusBar_();
    QWidget* buildSidebar_();
    QWidget* buildObjectArea_();
    QWidget* buildDetailPane_();
    void connectSettings_();
    void restoreWindowState_();
    void saveWindowState_();

    void refreshIcons_();
    void applyBrowseMode_();
    void applyThemeChoice_();
    void showSettingsDialog_();
    void showAddAccountDialog_();
    void refreshAccountSummary_();
    void showAbout_();

    SettingsController* m_settings;
    ThemeManager*       m_theme;
    AccountStore*       m_accounts;
    SecretStore*        m_secrets;
    AccountSessions*    m_sessions = nullptr;   // with account management only

    // Menu actions
    QMenu*        m_viewMenu        = nullptr;
    QAction*      m_actAddAccount   = nullptr;
    QAction*      m_actQuit         = nullptr;
    QActionGroup* m_browseModeGroup = nullptr;
    QAction*      m_actFolders      = nullptr;
    QAction*      m_actFlat         = nullptr;
    QAction*      m_actVersions     = nullptr;
    QActionGroup* m_themeGroup      = nullptr;
    QAction*      m_actThemeDark    = nullptr;
    QAction*      m_actThemeLight   = nullptr;
    QAction*      m_actThemeSystem  = nullptr;
    QAction*      m_actSettings     = nullptr;
    QAction*      m_actAbout        = nullptr;
    QAction*      m_actAboutQt      = nullptr;

    // Toolbar
    QToolButton*  m_uploadBtn   = nullptr;
    QToolButton*  m_downloadBtn = nullptr;
    QToolButton*  m_newFolderBtn = nullptr;
    QToolButton*  m_deleteBtn   = nullptr;
    QToolButton*  m_backBtn     = nullptr;
    QToolButton*  m_forwardBtn  = nullptr;
    QToolButton*  m_upBtn       = nullptr;
    QToolButton*  m_refreshBtn  = nullptr;
    QLineEdit*    m_filterField = nullptr;
    QAction*      m_filterIcon  = nullptr;
    QButtonGroup* m_browseSeg   = nullptr;
    QToolButton*  m_themeBtn    = nullptr;
    QToolButton*  m_settingsBtn = nullptr;

    // Central
    QSplitter*      m_splitter      = nullptr;
    QTreeView*      m_accountTree   = nullptr;
    QLabel*         m_accountSummary = nullptr;
    QPushButton*    m_addAccountBtn  = nullptr;
    QStackedWidget* m_objectStack   = nullptr;
    QTreeView*      m_objectView    = nullptr;
    QLabel*         m_pathText      = nullptr;
    QDockWidget*    m_transferDock  = nullptr;

    // Status bar
    QLabel* m_locationLabel   = nullptr;
    QLabel* m_connectionDot   = nullptr;
    QLabel* m_connectionLabel = nullptr;
};

} // namespace qbucket
