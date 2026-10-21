#pragma once

#include <qbucket/s3/ListObjectVersionsPager.h>
#include <qbucket/s3/Types.h>

#include <QModelIndexList>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <memory>

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTreeWidget;

namespace qbucket {

class AccountSessions;
class CapabilityController;
class FolderSizeTask;
class OperationController;

// The right column (PLAN.md §4): Properties of the selected object
// (HeadObject, asked for lazily once the selection rests and dropped when
// it moves on), its headers and user metadata (edited through the Edit
// Metadata dialog), its tags and its access (canned ACL; the grants shown
// read-only) — both read only while their tab is shown, written through the
// OperationController, and hidden where the server has no such feature. A
// summary for a folder or several rows. The Versions tab lists the
// object's versions and delete markers (only while shown). A row of the
// versions view shows that version (HEAD, tags, ACL of it — editable only
// for the current one); a delete marker has nothing to ask for.
class DetailPane : public QWidget {
    Q_OBJECT

public:
    // How long the selection has to rest before an object is asked for.
    static constexpr int kHeadDelayMs = 150;

    explicit DetailPane(AccountSessions* sessions, QWidget* parent = nullptr);
    ~DetailPane() override;
    // Without them the tags and the access are shown read-only.
    void setControllers(OperationController* operations, CapabilityController* capabilities);

    enum Tab { PropertiesTab, MetadataTab, TagsTab, VersionsTab, AccessTab };
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }
    [[nodiscard]] QTableWidget* tagTable() const { return m_tagTable; }
    [[nodiscard]] QPushButton* applyTagsButton() const { return m_applyTags; }
    [[nodiscard]] QComboBox* cannedAclBox() const { return m_canned; }
    [[nodiscard]] QPushButton* applyAccessButton() const { return m_applyAccess; }
    [[nodiscard]] QTreeWidget* grantList() const { return m_grants; }
    [[nodiscard]] QTreeWidget* versionList() const { return m_versionList; }
    [[nodiscard]] QPushButton* downloadVersionButton() const { return m_versionDownload; }
    [[nodiscard]] QPushButton* restoreVersionButton() const { return m_versionRestore; }
    [[nodiscard]] QPushButton* deleteVersionButton() const { return m_versionDelete; }
    [[nodiscard]] QPushButton* removeMarkerButton() const { return m_versionUndelete; }
    // The version shown (a row of the versions view); empty: the current object.
    [[nodiscard]] QString versionId() const { return m_versionId; }
    // A folder (or several rows with folders): what they hold, on request.
    [[nodiscard]] QPushButton* sizeButton() const { return m_sizeButton; }
    [[nodiscard]] QLabel* sizeLabel() const { return m_sizeLabel; }

    // Rows of an ObjectTableModel (any column; one per row counts).
    void setSelection(const QUuid& account, const QString& bucket, const QModelIndexList& rows);
    void clear();

    // The object last shown (tests).
    [[nodiscard]] const s3::ObjectMeta& meta() const { return m_meta; }

    // The longest version list the tab shows.
    static constexpr int kMaxVersionsShown = 5000;

signals:
    void headFinished();
    void tagsLoaded();
    void accessLoaded();
    void versionsLoaded();
    // The Metadata tab's "Edit…".
    void editMetadataRequested();
    // The Versions tab's buttons, for the selected versions of one key.
    void downloadVersionsRequested(const QStringList& keys, const QStringList& versionIds);
    void restoreVersionsRequested(const QStringList& keys, const QStringList& versionIds);
    void deleteVersionsRequested(const QStringList& keys, const QStringList& versionIds);
    void removeMarkersRequested(const QStringList& keys);

private:
    void requestHead_();
    void showMeta_(const s3::ObjectMeta& meta);
    void showHint_(const QString& text);
    void addField_(const QString& label, const QString& value, bool selectable = true);
    void resetFields_();
    void buildTagsTab_();
    void buildAccessTab_();
    void buildVersionsTab_();
    // The tab shown (or the selection settled): load what it needs.
    void loadVisibleTab_();
    void loadTags_();
    void loadAccess_();
    void loadVersions_();
    void fetchVersions_();
    void showVersions_();
    void updateVersionButtons_();
    // Editing applies to the current version only.
    [[nodiscard]] bool showsOlderVersion_() const { return !m_versionId.isEmpty() && !m_latest; }
    void updateFeatureTabs_();
    void applyTags_();
    void applyAccess_();
    void addTagRow_(const QString& key, const QString& value);
    void addSizeRow_(const QStringList& prefixes);
    void startSize_(const QStringList& prefixes);

    AccountSessions* m_sessions;
    QUuid m_account;
    QString m_bucket;
    QString m_key;           // the object HEAD is asked for (not for a delete marker)
    QString m_versionId;     // that version (a row of the versions view); empty: current
    bool m_latest = true;    // the row is the key's current entry
    QString m_versionsKey;   // the object whose versions the tab lists
    s3::ObjectMeta m_meta;
    QTimer m_headDelay;
    std::unique_ptr<QObject> m_run;   // context of the pending HeadObject

    QTabWidget* m_tabs = nullptr;
    QStackedWidget* m_propsStack = nullptr;
    QLabel* m_propsHint = nullptr;
    QWidget* m_props = nullptr;
    QFormLayout* m_form = nullptr;
    QStackedWidget* m_metaStack = nullptr;
    QLabel* m_metaHint = nullptr;
    QTreeWidget* m_headers = nullptr;
    QPushButton* m_editMetadata = nullptr;

    OperationController* m_ops = nullptr;
    CapabilityController* m_caps = nullptr;
    QString m_tagsFor;       // key whose tags are shown / loading
    QString m_accessFor;
    std::unique_ptr<QObject> m_tagsRun;
    std::unique_ptr<QObject> m_accessRun;
    QStackedWidget* m_tagsStack = nullptr;
    QLabel* m_tagsHint = nullptr;
    QTableWidget* m_tagTable = nullptr;
    QPushButton* m_applyTags = nullptr;
    QLabel* m_tagsReason = nullptr;
    QStackedWidget* m_accessStack = nullptr;
    QLabel* m_accessHint = nullptr;
    QLabel* m_owner = nullptr;
    QTreeWidget* m_grants = nullptr;
    QComboBox* m_canned = nullptr;
    QPushButton* m_applyAccess = nullptr;
    QLabel* m_accessReason = nullptr;
    QString m_versionsFor;   // key whose versions are shown / loading
    std::unique_ptr<QObject> m_versionsRun;
    std::unique_ptr<s3::ListObjectVersionsPager> m_versionsPager;
    QList<s3::VersionEntry> m_versions;
    bool m_versionsCapped = false;
    QStackedWidget* m_versionsStack = nullptr;
    QLabel* m_versionsHint = nullptr;
    QTreeWidget* m_versionList = nullptr;
    QLabel* m_versionsNote = nullptr;
    QPushButton* m_versionDownload = nullptr;
    QPushButton* m_versionRestore = nullptr;
    QPushButton* m_versionDelete = nullptr;
    QPushButton* m_versionUndelete = nullptr;
    std::unique_ptr<FolderSizeTask> m_sizeTask;
    QPushButton* m_sizeButton = nullptr;   // in the form; gone with its rows
    QLabel* m_sizeLabel = nullptr;
};

} // namespace qbucket
