#pragma once

#include "domain/Account.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace qbucket {

class AccountStore;
class ConnectionTester;
class SecretStore;

// Add or edit an S3 account: provider preset (Hetzner first), region →
// endpoint, credentials, safety settings, and a "Test connection" button.
// Saving writes the secret to the keychain first, then the account — so an
// account never exists without its secret. Asynchronous throughout; open
// it with open(), never exec().
class AccountDialog : public QDialog {
    Q_OBJECT

public:
    // `existing` empty: a new account.
    AccountDialog(AccountStore* accounts, SecretStore* secrets,
                  const std::optional<Account>& existing = std::nullopt,
                  QWidget* parent = nullptr);
    ~AccountDialog() override;

    // The account as currently entered (id kept when editing).
    [[nodiscard]] Account account() const;

signals:
    void accountSaved(const qbucket::Account& account, bool secretSessionOnly);

private:
    void buildUi_();
    void fillProviders_();
    void onProviderChanged_();
    void onRegionChanged_();
    void onEnvironmentChanged_();
    void updateButtons_();
    void testConnection_();
    void save_();
    void showStatus_(const QString& text, bool error);

    AccountStore* m_accounts;
    SecretStore*  m_secrets;
    ConnectionTester* m_tester;
    Account       m_account;
    bool          m_editing = false;
    bool          m_endpointTouched = false;
    bool          m_readOnlyTouched = false;
    bool          m_saving = false;

    QLineEdit*    m_name = nullptr;
    QComboBox*    m_provider = nullptr;
    QComboBox*    m_region = nullptr;
    QLineEdit*    m_endpoint = nullptr;
    QComboBox*    m_addressing = nullptr;
    QLineEdit*    m_accessKey = nullptr;
    QLineEdit*    m_secret = nullptr;
    QComboBox*    m_environment = nullptr;
    QCheckBox*    m_readOnly = nullptr;
    QLineEdit*    m_externalBuckets = nullptr;
    QPushButton*  m_testBtn = nullptr;
    QLabel*       m_status = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace qbucket
