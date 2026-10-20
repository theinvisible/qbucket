#include "ui/AccountDialog.h"

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/ConnectionTester.h"

#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace qbucket {

AccountDialog::AccountDialog(AccountStore* accounts, SecretStore* secrets,
                             const std::optional<Account>& existing, QWidget* parent)
    : QDialog(parent),
      m_accounts(accounts),
      m_secrets(secrets),
      m_tester(new ConnectionTester(this)),
      m_editing(existing.has_value())
{
    Q_ASSERT(m_accounts && m_secrets);
    setWindowTitle(m_editing ? tr("Edit Account") : tr("Add Account"));
    setMinimumWidth(520);
    m_account = existing.value_or(Account{});
    if (!m_editing)
        m_account.id = QUuid::createUuid();

    buildUi_();
    fillProviders_();

    if (m_editing) {
        m_name->setText(m_account.name);
        m_provider->setCurrentIndex(qMax(0, m_provider->findData(m_account.providerId)));
        onProviderChanged_();
        m_region->setCurrentText(m_account.region);
        if (!m_account.endpoint.isEmpty()) {
            m_endpoint->setText(m_account.endpoint);
            m_endpointTouched = true;
        }
        m_addressing->setCurrentIndex(qMax(0, m_addressing->findData(int(m_account.addressing))));
        m_accessKey->setText(m_account.accessKeyId);
        m_secret->setPlaceholderText(tr("Unchanged — stored in the keychain"));
        m_environment->setCurrentIndex(qMax(0, m_environment->findData(int(m_account.environment))));
        m_readOnly->setChecked(m_account.readOnly);
        m_readOnlyTouched = true;
        m_externalBuckets->setText(m_account.externalBuckets.join(QStringLiteral(", ")));
    } else {
        onProviderChanged_();
        onEnvironmentChanged_();
    }

    connect(m_tester, &ConnectionTester::finished, this, [this](bool ok, const QString& message) {
        m_testBtn->setText(tr("Test Connection"));
        showStatus_(message, !ok);
        updateButtons_();
    });
    updateButtons_();
}

AccountDialog::~AccountDialog() = default;

void AccountDialog::buildUi_()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 12);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_name = new QLineEdit;
    m_name->setPlaceholderText(tr("e.g. Hetzner production"));
    form->addRow(tr("Name:"), m_name);

    m_provider = new QComboBox;
    form->addRow(tr("Provider:"), m_provider);

    m_region = new QComboBox;
    m_region->setEditable(true);
    form->addRow(tr("Region:"), m_region);

    m_endpoint = new QLineEdit;
    m_endpoint->setPlaceholderText(QStringLiteral("https://…"));
    form->addRow(tr("Endpoint:"), m_endpoint);

    m_addressing = new QComboBox;
    m_addressing->addItem(tr("Provider default"), int(s3::AddressingStyle::Auto));
    m_addressing->addItem(tr("Path-style"), int(s3::AddressingStyle::Path));
    m_addressing->addItem(tr("Virtual-hosted"), int(s3::AddressingStyle::VirtualHosted));
    form->addRow(tr("Addressing:"), m_addressing);

    m_accessKey = new QLineEdit;
    form->addRow(tr("Access key ID:"), m_accessKey);

    m_secret = new QLineEdit;
    m_secret->setEchoMode(QLineEdit::Password);
    m_secret->setPlaceholderText(tr("Stored in the system keychain"));
    form->addRow(tr("Secret access key:"), m_secret);

    m_environment = new QComboBox;
    m_environment->addItem(tr("Production"), int(Environment::Production));
    m_environment->addItem(tr("Staging"), int(Environment::Staging));
    m_environment->addItem(tr("Development"), int(Environment::Development));
    form->addRow(tr("Environment:"), m_environment);

    m_readOnly = new QCheckBox(tr("Read-only — never change or delete anything"));
    form->addRow(QString(), m_readOnly);

    m_externalBuckets = new QLineEdit;
    m_externalBuckets->setPlaceholderText(tr("Optional: buckets of other accounts, comma-separated"));
    form->addRow(tr("External buckets:"), m_externalBuckets);
    root->addLayout(form);

    auto* testRow = new QHBoxLayout;
    m_testBtn = new QPushButton(tr("Test Connection"));
    testRow->addWidget(m_testBtn);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("emptyState"));
    m_status->setWordWrap(true);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    testRow->addWidget(m_status, 1);
    root->addLayout(testRow);
    root->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    root->addWidget(m_buttons);

    connect(m_provider, &QComboBox::currentIndexChanged, this, &AccountDialog::onProviderChanged_);
    connect(m_region, &QComboBox::currentTextChanged, this, &AccountDialog::onRegionChanged_);
    connect(m_environment, &QComboBox::currentIndexChanged, this, &AccountDialog::onEnvironmentChanged_);
    connect(m_endpoint, &QLineEdit::textEdited, this, [this] { m_endpointTouched = true; updateButtons_(); });
    connect(m_readOnly, &QCheckBox::clicked, this, [this] { m_readOnlyTouched = true; });
    for (QLineEdit* e : {m_name, m_accessKey, m_secret})
        connect(e, &QLineEdit::textChanged, this, &AccountDialog::updateButtons_);
    connect(m_testBtn, &QPushButton::clicked, this, &AccountDialog::testConnection_);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &AccountDialog::save_);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AccountDialog::fillProviders_()
{
    // Registry order: Hetzner first, the preselected preset.
    for (const auto& p : s3::ProviderRegistry::builtin().profiles())
        m_provider->addItem(p->displayName(), p->id());
    m_provider->setCurrentIndex(0);
}

void AccountDialog::onProviderChanged_()
{
    const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(m_provider->currentData().toString());
    const QSignalBlocker block(m_region);
    m_region->clear();
    m_region->addItems(profile->regions());
    if (profile->regions().isEmpty())
        m_region->setEditText(profile->signingRegion(QString()));
    m_endpointTouched = false;
    onRegionChanged_();
}

void AccountDialog::onRegionChanged_()
{
    if (m_endpointTouched)
        return;
    const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(m_provider->currentData().toString());
    const QUrl url = profile->endpointFor(m_region->currentText().trimmed());
    m_endpoint->setText(url.isValid() ? url.toString() : QString());
    updateButtons_();
}

void AccountDialog::onEnvironmentChanged_()
{
    // Production accounts start read-only; the user may still switch it off.
    if (!m_readOnlyTouched)
        m_readOnly->setChecked(Environment(m_environment->currentData().toInt()) == Environment::Production);
}

Account AccountDialog::account() const
{
    Account a = m_account;
    a.name = m_name->text().trimmed();
    a.providerId = m_provider->currentData().toString();
    a.region = m_region->currentText().trimmed();
    // Only a deliberately entered endpoint is stored; otherwise the
    // provider's endpoint for the region follows profile updates.
    a.endpoint = m_endpointTouched ? m_endpoint->text().trimmed() : QString();
    a.addressing = s3::AddressingStyle(m_addressing->currentData().toInt());
    a.accessKeyId = m_accessKey->text().trimmed();
    a.environment = Environment(m_environment->currentData().toInt());
    a.readOnly = m_readOnly->isChecked();
    a.externalBuckets.clear();
    for (const QString& b : m_externalBuckets->text().split(u',', Qt::SkipEmptyParts)) {
        if (!b.trimmed().isEmpty())
            a.externalBuckets.append(b.trimmed());
    }
    return a;
}

void AccountDialog::updateButtons_()
{
    const bool haveSecret = !m_secret->text().isEmpty() || m_editing;
    const bool endpointOk = !m_endpoint->text().trimmed().isEmpty();
    const bool complete = !m_name->text().trimmed().isEmpty() && !m_accessKey->text().trimmed().isEmpty()
                       && haveSecret && endpointOk;
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(complete && !m_saving);
    m_testBtn->setEnabled(!m_accessKey->text().trimmed().isEmpty() && !m_secret->text().isEmpty()
                          && endpointOk && !m_saving);
}

void AccountDialog::showStatus_(const QString& text, bool error)
{
    m_status->setText(text);
    m_status->setProperty("error", error);
    m_status->setToolTip(text);
}

void AccountDialog::testConnection_()
{
    if (m_tester->isRunning()) {
        m_tester->cancel();
        m_testBtn->setText(tr("Test Connection"));
        showStatus_(tr("Cancelled."), false);
        return;
    }
    Account a = account();
    a.endpoint = m_endpoint->text().trimmed();   // test exactly what is shown
    m_testBtn->setText(tr("Cancel Test"));
    showStatus_(tr("Connecting…"), false);
    m_tester->start(a, m_secret->text());
}

void AccountDialog::save_()
{
    const Account a = account();
    if (!a.isValid()) {
        showStatus_(tr("Name, provider and access key ID are required."), true);
        return;
    }
    m_tester->cancel();
    m_saving = true;
    updateButtons_();

    const auto storeAccount = [this, a](bool sessionOnly) {
        m_saving = false;
        if (!m_accounts->save(a)) {
            showStatus_(tr("The account could not be saved."), true);
            updateButtons_();
            return;
        }
        emit accountSaved(a, sessionOnly);
        accept();
    };

    if (m_secret->text().isEmpty()) {   // editing, secret unchanged
        storeAccount(false);
        return;
    }
    showStatus_(tr("Saving the secret in the keychain…"), false);
    m_secrets->write(a.id, SecretStore::Kind::SecretAccessKey, m_secret->text(), this,
                     [this, storeAccount](const SecretStore::Result& r) {
        if (!r.ok()) {
            m_saving = false;
            showStatus_(tr("The keychain refused the secret: %1").arg(r.message), true);
            updateButtons_();
            return;
        }
        storeAccount(r.sessionOnly);
    });
}

} // namespace qbucket
