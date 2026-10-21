#include "ui/CreateBucketDialog.h"

#include "accounts/AccountStore.h"
#include "controllers/CapabilityController.h"
#include "controllers/OperationController.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace qbucket {

CreateBucketDialog::CreateBucketDialog(OperationController* operations, const QUuid& account, QWidget* parent)
    : QDialog(parent),
      m_ops(operations),
      m_account(account)
{
    QString accountName;
    if (m_ops) {
        if (const std::optional<Account> a = m_ops->accounts()->account(account)) {
            accountName = a->name;
            m_region = a->region;
        }
    }
    setWindowTitle(tr("New Bucket — %1").arg(accountName));
    setMinimumWidth(460);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("bucketName"));
    m_name->setPlaceholderText(tr("lower-case letters, digits, dots, hyphens"));
    form->addRow(tr("Name:"), m_name);
    auto* region = new QLabel(m_region.isEmpty() ? tr("the endpoint's") : m_region);
    region->setToolTip(tr("The bucket is created where the account's endpoint is."));
    form->addRow(tr("Region:"), region);
    lay->addLayout(form);
    m_problem = new QLabel;
    m_problem->setObjectName(QStringLiteral("bucketNameProblem"));
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::dangerColor().name()));
    lay->addWidget(m_problem);

    m_lock = new QCheckBox(tr("Object lock (can't be switched off again; turns versioning on)"));
    m_lock->setObjectName(QStringLiteral("bucketObjectLock"));
    m_versioning = new QCheckBox(tr("Versioning"));
    m_versioning->setObjectName(QStringLiteral("bucketVersioning"));
    lay->addWidget(m_versioning);
    lay->addWidget(m_lock);
    if (m_ops) {
        if (m_ops->capabilities()->decide(account, QString(), s3::Feature::ObjectLock, false) == s3::Decision::Blocked) {
            m_lock->setEnabled(false);
            m_lock->setToolTip(CapabilityController::reason(
                s3::Feature::ObjectLock, m_ops->capabilities()->entry(account, QString(), s3::Feature::ObjectLock), false));
        }
        const OperationController::Check gate =
            m_ops->check(account, QString(), {s3::Feature::BucketVersioningWrite}, true);
        m_versioningAllowed = gate.allowed;
        if (!gate.allowed)
            m_versioning->setToolTip(gate.reason);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_create = buttons->addButton(tr("Create…"), QDialogButtonBox::AcceptRole);
    m_create->setDefault(true);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_name, &QLineEdit::textChanged, this, &CreateBucketDialog::update_);
    connect(m_lock, &QCheckBox::toggled, this, &CreateBucketDialog::update_);
    update_();
}

QString CreateBucketDialog::name() const
{
    return m_name->text().trimmed();
}

CreateBucketJob::Options CreateBucketDialog::options() const
{
    CreateBucketJob::Options o;
    o.region = m_region;
    o.objectLock = m_lock->isEnabled() && m_lock->isChecked();
    o.versioning = o.objectLock || (m_versioning->isEnabled() && m_versioning->isChecked());
    return o;
}

void CreateBucketDialog::update_()
{
    const QString problem = name().isEmpty() ? QString() : CreateBucketJob::nameProblem(name());
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    if (m_lock->isChecked()) {
        m_versioning->setChecked(true);
        m_versioning->setEnabled(false);
    } else {
        m_versioning->setEnabled(m_versioningAllowed);
        if (!m_versioningAllowed)
            m_versioning->setChecked(false);
    }
    m_create->setEnabled(!name().isEmpty() && problem.isEmpty());
}

} // namespace qbucket
