#include "ui/OperationPreviewDialog.h"

#include "controllers/Job.h"
#include "ui/AccountTreeDelegate.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace qbucket {

OperationPreviewDialog::OperationPreviewDialog(Job* job, QWidget* parent)
    : QDialog(parent),
      m_job(job)
{
    const Job::Preview& p = job->preview();
    const Account& account = job->account();
    setWindowTitle(tr("%1 — %2").arg(p.action, account.name));
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(520);
    auto* lay = new QVBoxLayout(this);

    // Where: environment badge, account, scope.
    auto* where = new QHBoxLayout;
    auto* badge = new QLabel(AccountTreeDelegate::environmentLabel(int(account.environment)));
    badge->setObjectName(QStringLiteral("envBadge"));
    const QColor env = AccountTreeDelegate::environmentColor(int(account.environment));
    badge->setStyleSheet(QStringLiteral("QLabel#envBadge { color: %1; border: 1px solid %1; }").arg(env.name()));
    where->addWidget(badge);
    auto* accountLabel = new QLabel(account.name);
    accountLabel->setObjectName(QStringLiteral("fieldLabel"));
    where->addWidget(accountLabel);
    where->addStretch(1);
    lay->addLayout(where);

    auto* scope = new QLabel(p.scope);
    scope->setObjectName(QStringLiteral("previewScope"));
    scope->setTextInteractionFlags(Qt::TextSelectableByMouse);
    scope->setWordWrap(true);
    QFont bold = scope->font();
    bold.setBold(true);
    scope->setFont(bold);
    lay->addWidget(scope);

    // How much.
    const QLocale locale;
    if (p.objects >= 0) {   // -1: a setting, no objects
        QString amount = tr("%n object(s)", nullptr, int(p.objects));
        if (p.bytes >= 0)
            amount = tr("%1, %2").arg(amount, locale.formattedDataSize(p.bytes));
        lay->addWidget(new QLabel(amount));
    }
    for (const QString& d : p.details) {
        auto* line = new QLabel(d);
        line->setWordWrap(true);
        lay->addWidget(line);
    }
    if (!p.samples.isEmpty()) {
        auto* list = new QListWidget;
        list->setObjectName(QStringLiteral("previewSamples"));
        list->addItems(p.samples);
        if (p.objects > p.samples.size())
            list->addItem(tr("… and %n more", nullptr, int(p.objects - p.samples.size())));
        list->setMaximumHeight(160);
        list->setSelectionMode(QAbstractItemView::NoSelection);
        lay->addWidget(list);
    }

    // Targets that exist already, and what to do with them.
    if (p.conflicts > 0) {
        setMinimumWidth(720);   // both sides of a conflict on one line
        auto* head = new QLabel(tr("%n of them exist already:", nullptr, int(p.conflicts)));
        head->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::warningColor().name()));
        lay->addWidget(head);
        auto* conflicts = new QListWidget;
        conflicts->setObjectName(QStringLiteral("previewConflicts"));
        conflicts->addItems(p.conflictSamples);
        if (p.conflicts > p.conflictSamples.size())
            conflicts->addItem(tr("… and %n more", nullptr, int(p.conflicts - p.conflictSamples.size())));
        conflicts->setMaximumHeight(140);
        conflicts->setSelectionMode(QAbstractItemView::NoSelection);
        lay->addWidget(conflicts);
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Existing objects:")));
        m_policy = new QComboBox;
        m_policy->setObjectName(QStringLiteral("conflictPolicy"));
        const auto offered = [&p](Job::ConflictPolicy policy) {
            return p.policies.isEmpty() || p.policies.contains(policy);
        };
        if (offered(Job::ConflictPolicy::Skip))
            m_policy->addItem(tr("Skip them"), int(Job::ConflictPolicy::Skip));
        if (offered(Job::ConflictPolicy::Overwrite))
            m_policy->addItem(tr("Overwrite them"), int(Job::ConflictPolicy::Overwrite));
        if (offered(Job::ConflictPolicy::KeepBoth)) {
            m_policy->addItem(p.keepBothLabel.isEmpty() ? tr("Keep both (upload as “name (1)”)") : p.keepBothLabel,
                              int(Job::ConflictPolicy::KeepBoth));
        }
        m_policy->setCurrentIndex(std::max(0, m_policy->findData(int(job->conflictPolicy()))));
        row->addWidget(m_policy, 1);
        lay->addLayout(row);
    }

    // Consequences.
    const auto warn = [lay](const QString& text, const QColor& color) {
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
        lay->addWidget(label);
    };
    for (const QString& w : p.warnings)
        warn(w, Theme::warningColor());
    if (p.permanent)
        warn(tr("This cannot be undone: the bucket keeps no earlier versions."), Theme::dangerColor());

    // Extra steps.
    if (account.environment == Environment::Production) {
        m_production = new QCheckBox(tr("This is a production account — I have checked the scope above."));
        lay->addWidget(m_production);
        connect(m_production, &QCheckBox::toggled, this, &OperationPreviewDialog::updateButton_);
    }
    if (!p.typeToConfirm.isEmpty()) {
        lay->addWidget(new QLabel(tr("Type “%1” to confirm:").arg(p.typeToConfirm)));
        m_typed = new QLineEdit;
        m_typed->setObjectName(QStringLiteral("typedConfirmation"));
        lay->addWidget(m_typed);
        connect(m_typed, &QLineEdit::textChanged, this, &OperationPreviewDialog::updateButton_);
    }

    auto* buttons = new QDialogButtonBox;
    QPushButton* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    m_confirm = buttons->addButton(p.action, QDialogButtonBox::AcceptRole);
    m_confirm->setObjectName(QStringLiteral("danger"));
    m_confirm->setAutoDefault(false);
    cancel->setDefault(true);
    cancel->setFocus();
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this](int result) {
        if (m_answered || !m_job)
            return;
        m_answered = true;
        if (result == QDialog::Accepted) {
            if (m_policy)
                m_job->setConflictPolicy(Job::ConflictPolicy(m_policy->currentData().toInt()));
            m_job->confirm();
        } else {
            m_job->cancel();
        }
    });
    // Ended elsewhere (refused, cancelled): nothing left to confirm.
    connect(job, &Job::finished, this, [this] {
        m_answered = true;
        reject();
    });
    updateButton_();
}

void OperationPreviewDialog::updateButton_()
{
    bool ok = true;
    if (m_production)
        ok = ok && m_production->isChecked();
    if (m_typed && m_job)
        ok = ok && m_typed->text() == m_job->preview().typeToConfirm;
    m_confirm->setEnabled(ok);
}

} // namespace qbucket
