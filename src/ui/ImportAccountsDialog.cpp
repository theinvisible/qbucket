#include "ui/ImportAccountsDialog.h"

#include "accounts/AccountStore.h"
#include "controllers/ConnectionTester.h"

#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace qbucket {

ImportAccountsDialog::ImportAccountsDialog(AccountStore* accounts, SecretStore* secrets, QWidget* parent)
    : QDialog(parent),
      m_accounts(accounts),
      m_secrets(secrets),
      m_importer(new AccountImporter(accounts, secrets, this)),
      m_tester(new ConnectionTester(this))
{
    setWindowTitle(tr("Import Accounts"));
    setMinimumSize(900, 460);
    auto* lay = new QVBoxLayout(this);

    auto* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(new QLabel(tr("From:")));
    m_sourceGroup = new QButtonGroup(this);
    auto* aws = new QRadioButton(tr("AWS CLI (~/.aws)"));
    auto* rclone = new QRadioButton(tr("rclone"));
    m_sourceGroup->addButton(aws, int(AccountImporter::Source::Aws));
    m_sourceGroup->addButton(rclone, int(AccountImporter::Source::Rclone));
    aws->setChecked(true);
    sourceRow->addWidget(aws);
    sourceRow->addWidget(rclone);
    sourceRow->addStretch(1);
    auto* choose = new QPushButton(tr("Choose File…"));
    sourceRow->addWidget(choose);
    lay->addLayout(sourceRow);
    m_files = new QLabel;
    m_files->setObjectName(QStringLiteral("fieldLabel"));
    m_files->setWordWrap(true);
    lay->addWidget(m_files);

    m_table = new QTableWidget(0, 6);
    m_table->setObjectName(QStringLiteral("importCandidates"));
    m_table->setHorizontalHeaderLabels({QString(), tr("Name"), tr("Provider"), tr("Region"), tr("Endpoint"), tr("Status")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(ImportColumn, 28);
    m_table->setColumnWidth(NameColumn, 160);
    m_table->setColumnWidth(ProviderColumn, 220);
    m_table->setColumnWidth(RegionColumn, 100);
    m_table->setColumnWidth(EndpointColumn, 200);
    lay->addWidget(m_table, 1);

    auto* form = new QFormLayout;
    m_environment = new QComboBox;
    m_environment->addItem(tr("Production"), int(Environment::Production));
    m_environment->addItem(tr("Staging"), int(Environment::Staging));
    m_environment->addItem(tr("Development"), int(Environment::Development));
    form->addRow(tr("Environment:"), m_environment);
    m_readOnly = new QCheckBox(tr("Read-only — never change or delete anything"));
    m_readOnly->setChecked(true);
    form->addRow(QString(), m_readOnly);
    lay->addLayout(form);

    m_hint = new QLabel;
    m_hint->setObjectName(QStringLiteral("fieldLabel"));
    m_hint->setWordWrap(true);
    lay->addWidget(m_hint);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("emptyState"));
    m_status->setWordWrap(true);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lay->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_test = buttons->addButton(tr("Test Selected"), QDialogButtonBox::ActionRole);
    m_import = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    m_import->setObjectName(QStringLiteral("primary"));
    lay->addWidget(buttons);

    connect(m_sourceGroup, &QButtonGroup::idClicked, this,
            [this](int id) { load(AccountImporter::Source(id)); });
    connect(choose, &QPushButton::clicked, this, &ImportAccountsDialog::chooseFiles_);
    connect(m_environment, &QComboBox::currentIndexChanged, this, [this] {
        // Production accounts start read-only, as in the account dialog.
        if (!m_readOnlyTouched)
            m_readOnly->setChecked(Environment(m_environment->currentData().toInt()) == Environment::Production);
    });
    connect(m_readOnly, &QCheckBox::clicked, this, [this] { m_readOnlyTouched = true; });
    connect(m_table, &QTableWidget::itemChanged, this, &ImportAccountsDialog::updateButtons_);
    connect(m_test, &QPushButton::clicked, this, [this] {
        if (m_testing >= 0) {
            m_testQueue.clear();
            m_tester->cancel();
            m_testing = -1;
            updateButtons_();
            return;
        }
        for (int row = 0; row < m_table->rowCount(); ++row) {
            if (m_table->item(row, ImportColumn)->checkState() == Qt::Checked)
                m_testQueue.append(row);
        }
        testNext_();
    });
    connect(m_tester, &ConnectionTester::finished, this, [this](bool ok, const QString& message) {
        if (m_testing >= 0 && m_testing < m_table->rowCount()) {
            QTableWidgetItem* status = m_table->item(m_testing, StatusColumn);
            status->setText(ok ? tr("Connected.") : message);
            status->setToolTip(message);
        }
        testNext_();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &ImportAccountsDialog::import_);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_importer, &AccountImporter::candidatesReady, this, &ImportAccountsDialog::showCandidates_);
    connect(m_importer, &AccountImporter::saveFinished, this,
            [this](int saved, bool sessionOnly, const QStringList& errors) {
        if (saved > 0)
            emit imported(saved, sessionOnly);
        if (errors.isEmpty()) {
            accept();
            return;
        }
        m_status->setText(errors.join(u'\n'));
        m_status->setProperty("error", true);
        updateButtons_();
    });

    load(AccountImporter::Source::Aws);
}

ImportAccountsDialog::~ImportAccountsDialog() = default;

void ImportAccountsDialog::load(AccountImporter::Source source, const QStringList& files)
{
    m_source = source;
    if (QAbstractButton* b = m_sourceGroup->button(int(source)))
        b->setChecked(true);
    const QStringList read = files.isEmpty() ? AccountImporter::defaultFiles(source) : files;
    QStringList shown;
    for (const QString& f : read)
        shown.append(QDir::toNativeSeparators(f));
    m_files->setText(read.isEmpty()
                         ? tr("No configuration file is visible to QBucket here (a sandbox hides them) — "
                              "choose the file.")
                         : tr("Read: %1").arg(shown.join(QStringLiteral(", "))));
    m_table->setRowCount(0);
    m_candidates.clear();
    m_status->setText(tr("Reading…"));
    m_status->setProperty("error", false);
    updateButtons_();
    m_request = m_importer->read(source, read);
}

void ImportAccountsDialog::chooseFiles_()
{
    auto* dlg = new QFileDialog(this, tr("Choose the Configuration File"));
    dlg->setFileMode(m_source == AccountImporter::Source::Aws ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);
    dlg->setOption(QFileDialog::DontUseNativeDialog, false);
    dlg->setFilter(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QFileDialog::filesSelected, this, [this](const QStringList& files) { load(m_source, files); });
    dlg->open();
}

void ImportAccountsDialog::showCandidates_(const QList<ImportCandidate>& candidates, const QString& problem,
                                           int request)
{
    if (request != m_request)
        return;   // an earlier load, overtaken
    m_candidates = candidates;
    const QSignalBlocker block(m_table);
    m_table->setRowCount(int(candidates.size()));
    const auto& registry = s3::ProviderRegistry::builtin();
    for (int row = 0; row < candidates.size(); ++row) {
        const ImportCandidate& c = candidates.at(row);
        auto* check = new QTableWidgetItem;
        const bool usable = c.importable();
        check->setFlags(usable ? Qt::ItemIsUserCheckable | Qt::ItemIsEnabled : Qt::NoItemFlags);
        check->setCheckState(usable && c.existing.isNull() ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, ImportColumn, check);
        auto* name = new QTableWidgetItem(c.name);
        name->setToolTip(c.origin);
        if (!usable)
            name->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(row, NameColumn, name);
        auto* provider = new QComboBox;
        for (const auto& p : registry.profiles())
            provider->addItem(p->displayName(), p->id());
        provider->setCurrentIndex(std::max(0, provider->findData(c.providerId)));
        provider->setToolTip(c.why);
        provider->setEnabled(usable);
        connect(provider, &QComboBox::currentIndexChanged, this, &ImportAccountsDialog::updateButtons_);
        m_table->setCellWidget(row, ProviderColumn, provider);
        auto* region = new QTableWidgetItem(c.region);
        region->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(row, RegionColumn, region);
        auto* endpoint = new QTableWidgetItem(c.endpoint.isEmpty() ? tr("(preset)") : c.endpoint);
        endpoint->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(row, EndpointColumn, endpoint);
        QString status = c.skipReason;
        if (status.isEmpty() && !c.existing.isNull()) {
            const std::optional<Account> a = m_accounts->account(c.existing);
            status = tr("Already set up as “%1”.").arg(a ? a->name : QString());
        }
        if (status.isEmpty())
            status = c.why;
        auto* statusItem = new QTableWidgetItem(status);
        statusItem->setFlags(Qt::ItemIsEnabled);
        statusItem->setToolTip(status);
        m_table->setItem(row, StatusColumn, statusItem);
    }
    m_status->setText(problem);
    m_status->setProperty("error", !problem.isEmpty());
    updateButtons_();
}

ImportCandidate ImportAccountsDialog::candidateAt_(int row) const
{
    ImportCandidate c = m_candidates.at(row);
    c.name = m_table->item(row, NameColumn)->text().trimmed();
    if (const auto* provider = qobject_cast<QComboBox*>(m_table->cellWidget(row, ProviderColumn)))
        c.providerId = provider->currentData().toString();
    return c;
}

QList<ImportCandidate> ImportAccountsDialog::chosen_() const
{
    QList<ImportCandidate> out;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, ImportColumn)->checkState() == Qt::Checked)
            out.append(candidateAt_(row));
    }
    return out;
}

void ImportAccountsDialog::updateButtons_()
{
    const QList<ImportCandidate> chosen = chosen_();
    bool namesOk = true;
    bool generic = false;
    for (const ImportCandidate& c : chosen) {
        namesOk = namesOk && !c.name.isEmpty();
        generic = generic || c.providerId == QLatin1String("generic");
    }
    const bool busy = m_importer->isSaving();
    m_import->setEnabled(!chosen.isEmpty() && namesOk && !busy && m_testing < 0);
    m_import->setText(chosen.isEmpty() ? tr("Import") : tr("Import %n Account(s)", nullptr, int(chosen.size())));
    m_test->setEnabled(!busy && (m_testing >= 0 || !chosen.isEmpty()));
    m_test->setText(m_testing >= 0 ? tr("Stop Testing") : tr("Test Selected"));
    m_hint->setText(generic ? tr("Generic S3 knows no provider quirks — pick the provider where you know it.")
                            : QString());
}

void ImportAccountsDialog::testNext_()
{
    m_testing = -1;
    while (!m_testQueue.isEmpty()) {
        const int row = m_testQueue.takeFirst();
        if (row >= m_table->rowCount())
            continue;
        const ImportCandidate c = candidateAt_(row);
        m_testing = row;
        m_table->item(row, StatusColumn)->setText(tr("Connecting…"));
        m_tester->start(AccountImporter::accountFor(c, Environment::Production, true), c.secretAccessKey);
        break;
    }
    updateButtons_();
}

void ImportAccountsDialog::import_()
{
    const QList<ImportCandidate> chosen = chosen_();
    if (chosen.isEmpty() || m_importer->isSaving())
        return;
    m_status->setProperty("error", false);
    m_status->setText(tr("Saving the secrets in the keychain…"));
    m_importer->save(chosen, Environment(m_environment->currentData().toInt()), m_readOnly->isChecked());
    updateButtons_();
}

} // namespace qbucket
