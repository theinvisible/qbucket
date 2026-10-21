#pragma once

#include <qbucket/s3/Types.h>

#include <QDialog>
#include <QUuid>

#include <memory>
#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace qbucket {

class AccountSessions;
class CapabilityController;
class OperationController;

// A bucket's properties in the look of the settings (#prefSidebar /
// #prefCat): General (name, region, versioning, object lock) and Versioning
// (status, MFA delete, enable / suspend through SetBucketVersioningJob).
// Every write goes through the OperationController; what can't be done
// says why. M6 adds policy, CORS, lifecycle, … as further pages.
class BucketPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    enum Page { GeneralPage, VersioningPage };

    BucketPropertiesDialog(const QUuid& account, const QString& bucket, AccountSessions* sessions,
                           OperationController* operations, CapabilityController* capabilities,
                           QWidget* parent = nullptr);
    ~BucketPropertiesDialog() override;

    void showPage(Page page);
    [[nodiscard]] QPushButton* enableButton() const { return m_enable; }
    [[nodiscard]] QPushButton* suspendButton() const { return m_suspend; }
    [[nodiscard]] QLabel* versioningLabel() const { return m_status; }

signals:
    // The versioning was read (after opening and after a change).
    void versioningLoaded();
    // "Server Capabilities…": MainWindow opens that dialog.
    void capabilitiesRequested();

private:
    QWidget* buildGeneralPage_();
    QWidget* buildVersioningPage_();
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
    std::optional<s3::VersioningStatus> m_current;   // unset: not read
    bool m_running = false;
};

} // namespace qbucket
