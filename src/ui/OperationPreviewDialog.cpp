#include "ui/OperationPreviewDialog.h"

#include "controllers/Job.h"
#include "ui/AccountTreeDelegate.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
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
        QString amount = p.amount.isEmpty() ? tr("%n object(s)", nullptr, int(p.objects)) : p.amount;
        if (p.bytes >= 0)
            amount = tr("%1, %2").arg(amount, locale.formattedDataSize(p.bytes));
        lay->addWidget(new QLabel(amount));
    }
    for (const QString& d : p.details) {
        auto* line = new QLabel(d);
        line->setWordWrap(true);
        lay->addWidget(line);
    }
    if (!p.document.isEmpty() || !p.currentDocument.isEmpty()) {
        // A configuration change: the new document, and what is set now.
        auto* tabs = new QTabWidget;
        tabs->setObjectName(QStringLiteral("previewDocuments"));
        tabs->setDocumentMode(true);
        const auto page = [](const QString& text, const QString& name) {
            auto* view = new QPlainTextEdit(text);
            view->setObjectName(name);
            view->setReadOnly(true);
            view->setLineWrapMode(QPlainTextEdit::NoWrap);
            view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            return view;
        };
        tabs->addTab(page(p.document.isEmpty() ? tr("(nothing)") : p.document, QStringLiteral("previewNew")),
                     tr("New"));
        tabs->addTab(page(p.currentDocument.isEmpty() ? tr("(nothing is set)") : p.currentDocument,
                          QStringLiteral("previewCurrent")),
                     tr("Now"));
        tabs->setMinimumHeight(220);
        setMinimumWidth(640);
        lay->addWidget(tabs, 1);
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
    if (p.permanent) {
        warn(p.permanentText.isEmpty() ? tr("This cannot be undone: the bucket keeps no earlier versions.")
                                       : p.permanentText,
             Theme::dangerColor());
    }
    if (p.bypassOffer) {
        m_bypass = new QCheckBox(tr("Bypass GOVERNANCE retention for this operation"));
        m_bypass->setObjectName(QStringLiteral("bypassGovernance"));
        m_bypass->setToolTip(tr("Sends x-amz-bypass-governance-retention. COMPLIANCE retention and legal holds still "
                                "hold."));
        lay->addWidget(m_bypass);
        m_bypassWarning = new QLabel(tr("Versions protected by a GOVERNANCE retention are changed or deleted anyway. "
                                        "This is what the retention is meant to prevent — be sure it is wanted."));
        m_bypassWarning->setWordWrap(true);
        m_bypassWarning->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::dangerColor().name()));
        m_bypassWarning->setVisible(false);
        lay->addWidget(m_bypassWarning);
        connect(m_bypass, &QCheckBox::toggled, this, [this](bool on) {
            m_bypassWarning->setVisible(on);
            updateButton_();
        });
    }

    // Extra steps.
    if (account.environment == Environment::Production) {
        m_production = new QCheckBox(tr("This is a production account — I have checked the scope above."));
        lay->addWidget(m_production);
        connect(m_production, &QCheckBox::toggled, this, &OperationPreviewDialog::updateButton_);
    }
    if (!p.typeToConfirm.isEmpty() || p.bypassOffer) {
        m_typeLabel = new QLabel;
        lay->addWidget(m_typeLabel);
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
            m_job->setBypassGovernance(m_bypass && m_bypass->isChecked());
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

QString OperationPreviewDialog::expectedTyping() const
{
    if (!m_job)
        return {};
    QString name = m_job->preview().typeToConfirm;
    if (m_bypass && m_bypass->isChecked())
        return name.isEmpty() ? QStringLiteral("BYPASS") : QStringLiteral("BYPASS ") + name;
    return name;
}

void OperationPreviewDialog::updateButton_()
{
    bool ok = true;
    if (m_production)
        ok = ok && m_production->isChecked();
    const QString expected = expectedTyping();
    if (m_typed) {
        m_typeLabel->setText(tr("Type “%1” to confirm:").arg(expected));
        m_typeLabel->setVisible(!expected.isEmpty());
        m_typed->setVisible(!expected.isEmpty());
        ok = ok && m_typed->text() == expected;
    }
    m_confirm->setEnabled(ok);
}

} // namespace qbucket
