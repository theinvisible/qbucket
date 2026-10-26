#pragma once

#include "models/ObjectSelection.h"
#include <QMainWindow>
#include <QUuid>

class QAction;
class QActionGroup;
class QButtonGroup;
class QDockWidget;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QTreeView;

namespace qbucket {

class AccountSessions;
class ActivityLog;
class CapabilityController;
class CapabilityStore;
class Job;
class DragStaging;
class ExternalDrag;
class Journal;
class OpenFileService;
class OperationController;
class PresignService;
class TransferService;
class TransferModel;
class AccountSidebar;
class DetailPane;
class ObjectBrowser;
class AccountStore;
class LocationStore;
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
    ~MainWindow() override;

    // The Bucket menu (tests).
    [[nodiscard]] QAction* newBucketAction() const { return m_actNewBucket; }
    [[nodiscard]] QAction* bucketPropertiesAction() const { return m_actBucketProperties; }
    [[nodiscard]] QAction* deleteBucketAction() const { return m_actDeleteBucket; }
    [[nodiscard]] QAction* importAccountsAction() const { return m_actImportAccounts; }
    // Bookmarks (tests).
    [[nodiscard]] QAction* addBookmarkAction() const { return m_actAddBookmark; }
    [[nodiscard]] QMenu* bookmarksMenu() const { return m_bookmarksMenu; }
    [[nodiscard]] LocationStore* locations() const { return m_locations; }
    // Objects opened in another application (tests).
    [[nodiscard]] OpenFileService* openFiles() const { return m_openFiles; }
    [[nodiscard]] QFrame* changeBar() const { return m_changeBar; }

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
    void connectBrowser_();
    void updateNavigation_();
    void updateStatus_();
    void showCapabilities_();
    void showBucketProperties_(const QUuid& account, const QString& bucket);
    // Versions (M5).
    void restoreVersions_(const QStringList& keys, const QStringList& versionIds);
    void deleteVersions_(const QStringList& keys, const QStringList& versionIds);
    void removeMarkers_(const QStringList& keys);
    void deleteAllVersions_();
    void restoreDeleted_();
    void downloadVersions_(const QStringList& keys, const QStringList& versionIds, const QString& folder);
    void showIncompleteUploads_();
    // "New Bucket…" in this account; "Delete Bucket…": asks whether to
    // delete what is in it, then the job's preview asks the rest.
    void createBucket_(const QUuid& account);
    void deleteBucket_(const QUuid& account, const QString& bucket);
    void connectOperations_();
    void updateJobStatus_();
    void showJobReport_(Job* job);
    void updateMutationActions_();
    void newFolder_();
    void deleteSelection_();
    void chooseUploadFiles_();
    void chooseUploadFolder_();
    // Uploads local files / folders into the current location.
    void startUpload_(const QStringList& paths, bool alwaysConfirm);
    void startUploadTo_(const QUuid& account, const QString& bucket, const QString& prefix, const QStringList& paths,
                        bool alwaysConfirm);
    // Downloads the browser's selection into `folder` (empty: the settings' download folder).
    void downloadSelection_(const QString& folder);
    // Unfinished transfers of the last session: resume or discard them.
    void offerResume_();
    void renameSelection_();
    // Headers / metadata of the selection (one object: its values, loaded
    // first), and its storage class.
    void editMetadata_();
    void changeStorageClass_();
    void presign_();
    // Copy / move objects of the account within it (paste, drops). Between
    // accounts: later (refused with the reason).
    void startCopy_(const ObjectSelection& selection, const QUuid& account, const QString& bucket,
                    const QString& prefix, bool move);
    void restoreWindowState_();
    void saveWindowState_();

    void refreshIcons_();
    void applyBrowseMode_();
    void applyThemeChoice_();
    void showSettingsDialog_();
    void showAddAccountDialog_();
    void showImportAccountsDialog_();
    void showAbout_();
    // Bookmarks and recent locations.
    void addBookmark_();
    void fillBookmarksMenu_();
    void manageBookmarks_();
    void pruneLocations_();
    // Open in the default application; the bar for changed files.
    void connectOpenFiles_();
    void updateChangeBar_();
    void showOpenFiles_();
    void showFolder_(const QString& filePath);
    // Quitting with changed files that were not uploaded: false = ask first.
    bool openFilesSettled_();

    SettingsController* m_settings;
    ThemeManager*       m_theme;
    AccountStore*       m_accounts;
    SecretStore*        m_secrets;
    AccountSessions*    m_sessions = nullptr;   // with account management only
    ActivityLog*        m_activity = nullptr;
    CapabilityStore*    m_capStore = nullptr;
    CapabilityController* m_capabilities = nullptr;
    Journal*            m_journal = nullptr;
    OperationController* m_operations = nullptr;
    PresignService* m_presign = nullptr;
    TransferService* m_transfers = nullptr;
    TransferModel* m_transferModel = nullptr;
    LocationStore* m_locations = nullptr;
    OpenFileService* m_openFiles = nullptr;
    DragStaging* m_staging = nullptr;
    ExternalDrag* m_externalDrag = nullptr;   // a child of this window
    QFrame* m_changeBar = nullptr;
    QLabel* m_changeText = nullptr;
    QUuid m_changeId;
    bool m_quitDecided = false;
    QTabWidget* m_dockTabs = nullptr;

    // Menu actions
    QMenu*        m_editMenu        = nullptr;
    QMenu*        m_viewMenu        = nullptr;
    QAction*      m_actAddAccount   = nullptr;
    QAction*      m_actImportAccounts = nullptr;
    QAction*      m_actQuit         = nullptr;
    QActionGroup* m_browseModeGroup = nullptr;
    QAction*      m_actFolders      = nullptr;
    QAction*      m_actFlat         = nullptr;
    QAction*      m_actVersions     = nullptr;
    QAction*      m_actBack         = nullptr;
    QAction*      m_actForward      = nullptr;
    QAction*      m_actUp           = nullptr;
    QAction*      m_actRefresh      = nullptr;
    QAction*      m_actEditPath     = nullptr;
    QActionGroup* m_themeGroup      = nullptr;
    QAction*      m_actThemeDark    = nullptr;
    QAction*      m_actThemeLight   = nullptr;
    QAction*      m_actThemeSystem  = nullptr;
    QAction*      m_actCapabilities = nullptr;
    QAction*      m_actIncompleteUploads = nullptr;
    QAction*      m_actOpenFiles = nullptr;
    QAction*      m_actNewBucket = nullptr;
    QAction*      m_actBucketProperties = nullptr;
    QAction*      m_actDeleteBucket = nullptr;
    QMenu*        m_bookmarksMenu = nullptr;
    QMenu*        m_recentMenu = nullptr;
    QAction*      m_actAddBookmark = nullptr;
    QAction*      m_actManageBookmarks = nullptr;
    QAction*      m_actNewFolder    = nullptr;
    QAction*      m_actUploadFiles  = nullptr;
    QAction*      m_actUploadFolder = nullptr;
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
    AccountSidebar* m_sidebar       = nullptr;
    ObjectBrowser*  m_browser       = nullptr;
    DetailPane*     m_details       = nullptr;
    QDockWidget*    m_transferDock  = nullptr;

    // Status bar
    QLabel* m_locationLabel   = nullptr;
    QLabel* m_jobLabel        = nullptr;
    QProgressBar* m_jobProgress = nullptr;
    QToolButton* m_jobCancel  = nullptr;
    QLabel* m_connectionDot   = nullptr;
    QLabel* m_connectionLabel = nullptr;
};

} // namespace qbucket
