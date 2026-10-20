#include "ui/SettingsDialog.h"

#include "controllers/SettingsController.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace qbucket {

SettingsDialog::SettingsDialog(SettingsController* settings, QWidget* parent)
    : QDialog(parent),
      m_settings(settings)
{
    setWindowTitle(tr("Settings"));
    setMinimumSize(620, 380);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    m_sidebar = new QWidget;
    m_sidebar->setObjectName(QStringLiteral("prefSidebar"));
    m_sidebar->setFixedWidth(180);
    auto* sideLay = new QVBoxLayout(m_sidebar);
    sideLay->setContentsMargins(8, 10, 8, 10);
    sideLay->setSpacing(2);
    sideLay->addStretch(1);
    body->addWidget(m_sidebar);

    m_pages = new QStackedWidget;
    body->addWidget(m_pages, 1);
    root->addLayout(body, 1);

    addCategory_(tr("Appearance"), buildAppearancePage_());

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->setContentsMargins(12, 10, 12, 12);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SettingsDialog::addCategory_(const QString& label, QWidget* page)
{
    auto* sideLay = static_cast<QVBoxLayout*>(m_sidebar->layout());
    const int index = m_pages->addWidget(page);

    auto* btn = new QToolButton;
    btn->setObjectName(QStringLiteral("prefCat"));
    btn->setText(label);
    btn->setCheckable(true);
    btn->setAutoExclusive(true);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(btn, &QToolButton::clicked, this,
            [this, index] { m_pages->setCurrentIndex(index); });
    // Insert above the trailing stretch.
    sideLay->insertWidget(sideLay->count() - 1, btn);

    if (index == 0)
        btn->setChecked(true);
}

QWidget* SettingsDialog::buildAppearancePage_()
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 18);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_themeCombo = new QComboBox;
    m_themeCombo->addItem(tr("Dark"),            QStringLiteral("Dark"));
    m_themeCombo->addItem(tr("Light"),           QStringLiteral("Light"));
    m_themeCombo->addItem(tr("Follow system"),   QStringLiteral("System"));
    m_themeCombo->setCurrentIndex(
        qMax(0, m_themeCombo->findData(m_settings->themeName())));
    connect(m_themeCombo, &QComboBox::activated, this, [this](int index) {
        m_settings->setThemeName(m_themeCombo->itemData(index).toString());
    });
    form->addRow(tr("Theme:"), m_themeCombo);
    lay->addLayout(form);

    auto* hint = new QLabel(
        tr("“Follow system” switches between light and dark together with "
           "your desktop."));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("emptyState"));
    hint->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    hint->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(hint);

    lay->addStretch(1);
    return page;
}

} // namespace qbucket
