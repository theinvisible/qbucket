#pragma once

#include <qbucket/s3/Types.h>

#include <QDialog>
#include <QUuid>

#include <memory>
#include <optional>

class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QWidget;

namespace qbucket {

class AccountSessions;
class AclSection;
class BucketConfigPage;
class CapabilityController;
class CorsPage;
class FolderSizeTask;
class LifecyclePage;
class ObjectLockPage;
class OperationController;
class OtherConfigPage;
class PolicyPage;
class PublicAccessBlockSection;

// A bucket's properties in the look of the settings (#prefSidebar /
// #prefCat): General (name, region, created, versioning, object lock, size
// on request), Versioning (status, MFA delete, enable / suspend through
// SetBucketVersioningJob), Policy, CORS, Lifecycle, Access (ACL, policy
// status, public access block), Object Lock (default retention) and More
// (what QBucket only shows). Each configuration page reads fresh the first
// time it is shown and writes only through the OperationController; what
// can't be done says why. Closing with changes not applied asks first.
class BucketPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    enum Page {
        GeneralPage,
        VersioningPage,
        PolicySettingsPage,
        CorsSettingsPage,
        LifecycleSettingsPage,
        AccessSettingsPage,
        ObjectLockSettingsPage,
        MoreSettingsPage,
    };

    BucketPropertiesDialog(const QUuid& account, const QString& bucket, AccountSessions* sessions,
                           OperationController* operations, CapabilityController* capabilities,
                           QWidget* parent = nullptr);
    ~BucketPropertiesDialog() override;

    void showPage(Page page);
    [[nodiscard]] QPushButton* enableButton() const { return m_enable; }
    [[nodiscard]] QPushButton* suspendButton() const { return m_suspend; }
    [[nodiscard]] QLabel* versioningLabel() const { return m_status; }
    [[nodiscard]] PolicyPage* policyPage() const { return m_policy; }
    [[nodiscard]] CorsPage* corsPage() const { return m_cors; }
    [[nodiscard]] LifecyclePage* lifecyclePage() const { return m_lifecycle; }
    [[nodiscard]] AclSection* aclSection() const { return m_acl; }
    [[nodiscard]] PublicAccessBlockSection* publicAccessBlockSection() const { return m_pab; }
    [[nodiscard]] ObjectLockPage* objectLockPage() const { return m_lock; }
    [[nodiscard]] OtherConfigPage* otherPage() const { return m_other; }
    [[nodiscard]] QLabel* policyStatusLabel() const { return m_policyStatus; }
    [[nodiscard]] QLabel* sizeLabel() const { return m_size; }
    [[nodiscard]] QPushButton* sizeButton() const { return m_sizeButton; }
    // A configuration edited and not applied.
    [[nodiscard]] bool hasUnsavedChanges() const;
    // Tests: close without asking.
    void setAskBeforeDiscarding(bool ask) { m_askBeforeDiscarding = ask; }

public slots:
    void reject() override;

signals:
    // The versioning was read (after opening and after a change).
    void versioningLoaded();
    // "Server Capabilities…": MainWindow opens that dialog.
    void capabilitiesRequested();

private:
    QWidget* buildGeneralPage_();
    QWidget* buildVersioningPage_();
    QWidget* buildAccessPage_();
    // A page's frame: title, a line about it, the content.
    QWidget* framed_(const QString& title, const QString& about, QWidget* content);
    void calculateSize_();
    void addCategory_(const QString& label, QWidget* page);
    void load_();
    void updateButtons_();
    void setVersioning_(s3::VersioningStatus status);

    QUuid m_account;
    QString m_bucket;
    AccountSessions* m_sessions;
    OperationController* m_ops;
    CapabilityController* m_caps;
    std::unique_ptr<QObject> m_run;

    QWidget* m_sidebar = nullptr;
    QStackedWidget* m_pages = nullptr;
    QLabel* m_region = nullptr;
    QLabel* m_generalVersioning = nullptr;
    QLabel* m_objectLock = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_mfa = nullptr;
    QLabel* m_reason = nullptr;
    QPushButton* m_enable = nullptr;
    QPushButton* m_suspend = nullptr;
    QLabel* m_created = nullptr;
    QLabel* m_size = nullptr;
    QPushButton* m_sizeButton = nullptr;
    FolderSizeTask* m_sizeTask = nullptr;
    PolicyPage* m_policy = nullptr;
    CorsPage* m_cors = nullptr;
    LifecyclePage* m_lifecycle = nullptr;
    AclSection* m_acl = nullptr;
    PublicAccessBlockSection* m_pab = nullptr;
    QLabel* m_policyStatus = nullptr;
    ObjectLockPage* m_lock = nullptr;
    OtherConfigPage* m_other = nullptr;
    bool m_askBeforeDiscarding = true;
    std::optional<s3::VersioningStatus> m_current;   // unset: not read
    bool m_running = false;
};

} // namespace qbucket
