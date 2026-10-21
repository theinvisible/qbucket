#include "ui/BucketConfigPage.h"

#include "accounts/AccountStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/SetBucketConfigJob.h"
#include "ui/Theme.h"

#include <qbucket/s3/Session.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace qbucket {

BucketConfigPage::BucketConfigPage(s3::BucketConfig kind, const Context& context, QWidget* parent)
    : QWidget(parent),
      m_kind(kind),
      m_context(context)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_state = new QLabel;
    m_state->setObjectName(QStringLiteral("configState"));
    QFont bold = m_state->font();
    bold.setBold(true);
    m_state->setFont(bold);
    lay->addWidget(m_state);
    m_note = new QLabel;
    m_note->setObjectName(QStringLiteral("hint"));
    m_note->setWordWrap(true);
    m_note->setVisible(false);
    lay->addWidget(m_note);
    m_editorArea = new QVBoxLayout;
    lay->addLayout(m_editorArea, 1);
    m_problem = new QLabel;
    m_problem->setObjectName(QStringLiteral("configProblem"));
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::dangerColor().name()));
    m_problem->setVisible(false);
    lay->addWidget(m_problem);
    auto* row = new QHBoxLayout;
    m_apply = new QPushButton(tr("Apply…"));
    m_apply->setObjectName(QStringLiteral("configApply"));
    m_revert = new QPushButton(tr("Revert"));
    m_revert->setObjectName(QStringLiteral("configRevert"));
    m_remove = new QPushButton(tr("Remove…"));
    m_remove->setObjectName(QStringLiteral("configRemove"));
    m_remove->setVisible(s3::bucketConfigDeleteOperation(kind).has_value());
    row->addWidget(m_apply);
    row->addWidget(m_revert);
    row->addStretch(1);
    row->addWidget(m_remove);
    lay->addLayout(row);
    m_reason = new QLabel;
    m_reason->setObjectName(QStringLiteral("hint"));
    m_reason->setWordWrap(true);
    m_reason->setVisible(false);
    lay->addWidget(m_reason);
    m_apply->setVisible(s3::bucketConfigPutOperation(kind).has_value());
    m_revert->setVisible(s3::bucketConfigPutOperation(kind).has_value());

    connect(m_apply, &QPushButton::clicked, this, [this] { startJob_(false); });
    connect(m_remove, &QPushButton::clicked, this, [this] { startJob_(true); });
    connect(m_revert, &QPushButton::clicked, this, [this] {
        m_filling = true;
        showDocument_(m_document);
        m_filling = false;
        update_();
    });
    if (m_context.capabilities) {
        connect(m_context.capabilities, &CapabilityController::capabilitiesUpdated, this,
                [this](const QUuid& a, const QString& b) {
                    if (a == m_context.account && (b.isEmpty() || b == m_context.bucket))
                        update_();
                });
    }
}

BucketConfigPage::~BucketConfigPage() = default;

void BucketConfigPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ensureLoaded();
}

void BucketConfigPage::ensureLoaded()
{
    if (!m_loaded && !m_loading)
        reload();
}

void BucketConfigPage::reload()
{
    const s3::Result<s3::Session*> session =
        m_context.sessions ? m_context.sessions->session(m_context.account)
                           : s3::Result<s3::Session*>(s3::Error::make(s3::ErrorCategory::Internal, tr("not connected")));
    if (!session) {
        m_loadError = ConnectionTester::describe(session.error());
        update_();
        return;
    }
    m_loading = true;
    m_run = std::make_unique<QObject>();
    update_();
    (*session)->getBucketConfig(m_context.bucket, m_kind, m_run.get(), [this](const s3::Result<s3::BucketConfigDocument>& r) {
        m_loading = false;
        m_loaded = bool(r);
        if (r) {
            m_document = *r;
            m_loadError.clear();
            m_filling = true;
            showDocument_(m_document);
            m_filling = false;
        } else {
            m_loadError = r.error().category == s3::ErrorCategory::Unsupported
                              ? tr("Not supported by this server.")
                              : (r.error().category == s3::ErrorCategory::NotPermitted
                                     ? tr("These credentials may not read it.")
                                     : ConnectionTester::describe(r.error()));
        }
        update_();
        emit loaded();
    });
}

bool BucketConfigPage::differs_() const
{
    const QByteArray edited = editedDocument_();
    if (!m_document.configured)
        return !edited.trimmed().isEmpty();
    return !s3::equivalentBucketConfig(m_kind, m_document.body, edited);
}

QString BucketConfigPage::stateText_(const s3::BucketConfigDocument& doc) const
{
    return doc.configured ? tr("Set") : tr("Not set");
}

bool BucketConfigPage::isDirty() const
{
    return m_loaded && differs_();
}

QString BucketConfigPage::problem_() const
{
    const QByteArray edited = editedDocument_();
    if (edited.trimmed().isEmpty())
        return s3::bucketConfigDeleteOperation(m_kind) ? tr("Nothing to apply — use Remove to take it away.")
                                                       : tr("Nothing to apply.");
    return SetBucketConfigJob::problem(m_kind, edited);
}

void BucketConfigPage::setNote_(const QString& note)
{
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
}

void BucketConfigPage::edited_()
{
    if (!m_filling)
        update_();
}

void BucketConfigPage::update_()
{
    // Can these credentials change it on this server at all?
    QString reason;
    const std::optional<s3::Feature> write = s3::bucketConfigWriteFeature(m_kind);
    if (!write) {
        reason = tr("QBucket shows this setting only.");
    } else if (!m_context.operations) {
        reason = tr("Read-only here.");
    } else {
        const OperationController::Check gate =
            m_context.operations->check(m_context.account, m_context.bucket, {*write}, true);
        if (!gate.allowed)
            reason = gate.reason;
    }
    m_readOnlyReason = reason;
    m_editable = m_loaded && !m_running && reason.isEmpty();
    setEditable_(m_editable);

    if (m_loading)
        m_state->setText(tr("Reading…"));
    else if (!m_loaded)
        m_state->setText(m_loadError);
    else
        m_state->setText(stateText_(m_document));

    const bool dirty = isDirty();
    const QString problem = m_editable && dirty ? problem_() : QString();
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    m_apply->setEnabled(m_editable && dirty && problem.isEmpty());
    m_revert->setEnabled(m_loaded && dirty && !m_running);
    m_remove->setEnabled(m_editable && m_document.configured);
    const QString why = m_running ? tr("A change is under way.") : (m_loaded ? reason : QString());
    m_reason->setText(why);
    m_reason->setVisible(!why.isEmpty());
    if (dirty != m_wasDirty) {
        m_wasDirty = dirty;
        emit dirtyChanged();
    }
}

void BucketConfigPage::startJob_(bool remove)
{
    if (!m_context.operations)
        return;
    const std::optional<Account> account = m_context.operations->accounts()->account(m_context.account);
    if (!account)
        return;
    Job* job = remove ? SetBucketConfigJob::remove(*account, m_context.bucket, m_kind)
                      : SetBucketConfigJob::put(*account, m_context.bucket, m_kind, editedDocument_(), editedCannedAcl_());
    m_running = true;
    update_();
    connect(job, &Job::finished, this, [this, job] {
        m_running = false;
        // Done: show what the server has now. Not done: keep the edit.
        if (job->outcome() == Job::Outcome::Succeeded)
            reload();
        else
            update_();
    });
    m_context.operations->start(job);
}

} // namespace qbucket
