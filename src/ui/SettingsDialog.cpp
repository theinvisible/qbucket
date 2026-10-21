#include "ui/SettingsDialog.h"

#include "controllers/SettingsController.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
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
    addCategory_(tr("Transfers"), buildTransfersPage_());

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

QWidget* SettingsDialog::buildTransfersPage_()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("transfersPage"));
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 18);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* dirRow = new QHBoxLayout;
    m_downloadDir = new QLineEdit(m_settings->downloadDirectory());
    m_downloadDir->setObjectName(QStringLiteral("downloadDirectory"));
    connect(m_downloadDir, &QLineEdit::editingFinished, this, [this] {
        m_settings->setDownloadDirectory(m_downloadDir->text());
        m_downloadDir->setText(m_settings->downloadDirectory());
    });
    auto* browse = new QPushButton(tr("Choose…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Download Folder"), m_settings->downloadDirectory());
        if (dir.isEmpty())
            return;
        m_settings->setDownloadDirectory(dir);
        m_downloadDir->setText(m_settings->downloadDirectory());
    });
    dirRow->addWidget(m_downloadDir, 1);
    dirRow->addWidget(browse);
    form->addRow(tr("Download folder:"), dirRow);

    m_parallelParts = new QSpinBox;
    m_parallelParts->setObjectName(QStringLiteral("parallelParts"));
    m_parallelParts->setRange(SettingsController::kMinParallelParts, SettingsController::kMaxParallelParts);
    m_parallelParts->setValue(m_settings->transferParallelParts());
    connect(m_parallelParts, &QSpinBox::valueChanged, m_settings, &SettingsController::setTransferParallelParts);
    form->addRow(tr("Parallel parts per account:"), m_parallelParts);

    m_threads = new QSpinBox;
    m_threads->setObjectName(QStringLiteral("transferThreads"));
    m_threads->setRange(SettingsController::kMinTransferThreads, SettingsController::kMaxTransferThreads);
    m_threads->setValue(m_settings->transferThreads());
    connect(m_threads, &QSpinBox::valueChanged, m_settings, &SettingsController::setTransferThreads);
    form->addRow(tr("Transfer threads per account:"), m_threads);

    auto* fetch = new QSpinBox;
    fetch->setObjectName(QStringLiteral("fetchConfirm"));
    fetch->setRange(SettingsController::kMinFetchConfirmMiB, SettingsController::kMaxFetchConfirmMiB);
    fetch->setSuffix(QStringLiteral(" MiB"));
    fetch->setValue(m_settings->fetchConfirmMiB());
    fetch->setToolTip(tr("Opening an object in another application or dragging it out downloads it first; "
                         "above this size QBucket asks."));
    connect(fetch, &QSpinBox::valueChanged, m_settings, &SettingsController::setFetchConfirmMiB);
    form->addRow(tr("Ask before fetching more than:"), fetch);
    lay->addLayout(form);

    auto* hint = new QLabel(
        tr("Parts are sent and fetched in parallel up to this limit; when a server signals overload, "
           "QBucket halves it and slowly grows it again. New transfer threads apply to connections "
           "opened from now on."));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("emptyState"));
    hint->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    lay->addWidget(hint);

    lay->addStretch(1);
    return page;
}

} // namespace qbucket
