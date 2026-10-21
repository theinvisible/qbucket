#pragma once

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
class OperationController;

// The right column (PLAN.md §4): Properties of the selected object
// (HeadObject, asked for lazily once the selection rests and dropped when
// it moves on), its headers and user metadata (edited through the Edit
// Metadata dialog), its tags and its access (canned ACL; the grants shown
// read-only) — both read only while their tab is shown, written through the
// OperationController, and hidden where the server has no such feature. A
// summary for a folder or several rows. Versions arrive with M5.
class DetailPane : public QWidget {
    Q_OBJECT

public:
    // How long the selection has to rest before an object is asked for.
    static constexpr int kHeadDelayMs = 150;

    explicit DetailPane(AccountSessions* sessions, QWidget* parent = nullptr);
    // Without them the tags and the access are shown read-only.
    void setControllers(OperationController* operations, CapabilityController* capabilities);

    enum Tab { PropertiesTab, MetadataTab, TagsTab, VersionsTab, AccessTab };
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }
    [[nodiscard]] QTableWidget* tagTable() const { return m_tagTable; }
    [[nodiscard]] QPushButton* applyTagsButton() const { return m_applyTags; }
    [[nodiscard]] QComboBox* cannedAclBox() const { return m_canned; }
    [[nodiscard]] QPushButton* applyAccessButton() const { return m_applyAccess; }
    [[nodiscard]] QTreeWidget* grantList() const { return m_grants; }

    // Rows of an ObjectTableModel (any column; one per row counts).
    void setSelection(const QUuid& account, const QString& bucket, const QModelIndexList& rows);
    void clear();

    // The object last shown (tests).
    [[nodiscard]] const s3::ObjectMeta& meta() const { return m_meta; }

signals:
    void headFinished();
    void tagsLoaded();
    void accessLoaded();
    // The Metadata tab's "Edit…".
    void editMetadataRequested();

private:
    void requestHead_();
    void showMeta_(const s3::ObjectMeta& meta);
    void showHint_(const QString& text);
    void addField_(const QString& label, const QString& value, bool selectable = true);
    void resetFields_();
    void buildTagsTab_();
    void buildAccessTab_();
    // The tab shown (or the selection settled): load what it needs.
    void loadVisibleTab_();
    void loadTags_();
    void loadAccess_();
    void updateFeatureTabs_();
    void applyTags_();
    void applyAccess_();
    void addTagRow_(const QString& key, const QString& value);

    AccountSessions* m_sessions;
    QUuid m_account;
    QString m_bucket;
    QString m_key;
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
};

} // namespace qbucket
