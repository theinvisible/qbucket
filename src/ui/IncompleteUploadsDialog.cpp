#include "ui/IncompleteUploadsDialog.h"

#include "accounts/AccountStore.h"
#include "controllers/AbortUploadsJob.h"
#include "controllers/OperationController.h"
#include "models/MultipartUploadModel.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

namespace qbucket {

IncompleteUploadsDialog::IncompleteUploadsDialog(OperationController* operations, TransferService* transfers,
                                                 const QUuid& account, const QString& bucket, QWidget* parent)
    : QDialog(parent),
      m_ops(operations),
      m_account(account),
      m_bucket(bucket),
      m_model(new MultipartUploadModel(operations->sessions(), transfers, account, bucket, this))
{
    setWindowTitle(tr("Incomplete Uploads — %1").arg(bucket));
    setMinimumSize(760, 420);
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Multipart uploads that were started and never completed. Their parts use space "
                                "until they are aborted."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    m_view = new QTableView;
    m_view->setObjectName(QStringLiteral("incompleteUploads"));
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->setColumnWidth(MultipartUploadModel::KeyColumn, 340);
    m_view->setColumnWidth(MultipartUploadModel::StartedColumn, 150);
    m_view->setColumnWidth(MultipartUploadModel::ClassColumn, 90);
    lay->addWidget(m_view, 1);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("fieldLabel"));
    lay->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_abort = buttons->addButton(tr("Abort Selected…"), QDialogButtonBox::ActionRole);
    m_abort->setObjectName(QStringLiteral("danger"));
    QPushButton* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refresh, &QPushButton::clicked, m_model, &MultipartUploadModel::reload);
    connect(m_abort, &QPushButton::clicked, this, &IncompleteUploadsDialog::abortSelected_);
    lay->addWidget(buttons);

    connect(m_model, &MultipartUploadModel::stateChanged, this, &IncompleteUploadsDialog::updateState_);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &IncompleteUploadsDialog::updateState_);
    m_model->reload();
    updateState_();
}

void IncompleteUploadsDialog::updateState_()
{
    switch (m_model->state()) {
    case MultipartUploadModel::State::Loading:
        m_status->setText(tr("Loading…"));
        break;
    case MultipartUploadModel::State::Error:
        m_status->setText(tr("Could not list them: %1").arg(m_model->errorText()));
        break;
    case MultipartUploadModel::State::Ready:
        if (m_model->rowCount() == 0)
            m_status->setText(tr("None — nothing takes space unseen."));
        else if (m_model->truncated())
            m_status->setText(tr("The first %n unfinished uploads.", nullptr, m_model->rowCount()));
        else
            m_status->setText(tr("%n unfinished upload(s).", nullptr, m_model->rowCount()));
        break;
    }
    const bool selected = m_view->selectionModel() && m_view->selectionModel()->hasSelection();
    const OperationController::Check gate = m_ops->check(m_account, m_bucket, {s3::Feature::MultipartUpload}, true);
    m_abort->setEnabled(selected && gate.allowed);
    m_abort->setToolTip(gate.allowed ? tr("Abort the selected uploads") : gate.reason);
}

void IncompleteUploadsDialog::abortSelected_()
{
    QList<AbortUploadsJob::Upload> uploads;
    for (const QModelIndex& row : m_view->selectionModel()->selectedRows()) {
        const s3::MultipartUploadInfo& u = m_model->upload(row.row());
        uploads.append({u.key, u.uploadId});
    }
    if (uploads.isEmpty())
        return;
    const std::optional<Account> account = m_ops->accounts()->account(m_account);
    if (!account)
        return;
    auto* job = new AbortUploadsJob(*account, m_bucket, uploads);
    connect(job, &Job::finished, m_model, &MultipartUploadModel::reload);
    m_ops->start(job);
}

} // namespace qbucket
