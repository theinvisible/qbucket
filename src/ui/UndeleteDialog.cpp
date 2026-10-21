#include "ui/UndeleteDialog.h"

#include <QCheckBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace qbucket {

UndeleteDialog::UndeleteDialog(const QString& scope, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Restore Deleted Objects"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Bring back the objects deleted in %1: the delete markers over their newest version "
                                "are removed. What comes back is shown before anything happens.")
                                 .arg(scope));
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* row = new QHBoxLayout;
    m_onlyAfter = new QCheckBox(tr("Only what was deleted after:"));
    m_after = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(-3600));
    m_after->setCalendarPopup(true);
    m_after->setEnabled(false);
    connect(m_onlyAfter, &QCheckBox::toggled, m_after, &QWidget::setEnabled);
    row->addWidget(m_onlyAfter);
    row->addWidget(m_after, 1);
    lay->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Continue…"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

QDateTime UndeleteDialog::deletedAfter() const
{
    return m_onlyAfter->isChecked() ? m_after->dateTime().toUTC() : QDateTime();
}

} // namespace qbucket
