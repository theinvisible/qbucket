#pragma once

#include "controllers/CreateBucketJob.h"

#include <QDialog>
#include <QUuid>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace qbucket {

class OperationController;

// "New Bucket": the name, checked as it is typed (S3's rules), the
// account's region, object lock (only possible now) and versioning — each
// switch off with the reason where the server or the account can't. The
// job itself (CreateBucketJob) is started by the caller with options().
class CreateBucketDialog : public QDialog {
    Q_OBJECT

public:
    CreateBucketDialog(OperationController* operations, const QUuid& account, QWidget* parent = nullptr);

    [[nodiscard]] QString name() const;
    [[nodiscard]] CreateBucketJob::Options options() const;
    [[nodiscard]] QLineEdit* nameEdit() const { return m_name; }
    [[nodiscard]] QLabel* problemLabel() const { return m_problem; }
    [[nodiscard]] QCheckBox* objectLockCheck() const { return m_lock; }
    [[nodiscard]] QCheckBox* versioningCheck() const { return m_versioning; }
    [[nodiscard]] QPushButton* createButton() const { return m_create; }

private:
    void update_();

    OperationController* m_ops;
    QUuid m_account;
    QString m_region;
    QLineEdit* m_name = nullptr;
    QLabel* m_problem = nullptr;
    QCheckBox* m_lock = nullptr;
    QCheckBox* m_versioning = nullptr;
    QPushButton* m_create = nullptr;
    bool m_versioningAllowed = false;
};

} // namespace qbucket
