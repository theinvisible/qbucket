#pragma once

#include "accounts/RcloneConfigReader.h"
#include "domain/Account.h"

#include <qbucket/s3/AwsConfigFiles.h>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUuid>

namespace qbucket {

class AccountStore;
class SecretStore;

// One account an import offers. Holds the secret key until it is saved:
// never logged, never shown.
struct ImportCandidate {
    QString origin;                       // "AWS profile “x”", "rclone remote “x”"
    QString name;                         // suggested account name, unique
    QString providerId;                   // suggested profile
    QString region;
    QString endpoint;                     // empty: the profile's endpoint for the region
    s3::AddressingStyle addressing = s3::AddressingStyle::Auto;
    QString accessKeyId;
    QString secretAccessKey;
    QString why;                          // how the profile was chosen, translated
    QString skipReason;                   // non-empty: can't be imported (translated)
    QUuid existing;                       // an account with the same endpoint and key
    [[nodiscard]] bool importable() const { return skipReason.isEmpty(); }
};

// Import of accounts from the AWS CLI's files and rclone's configuration
// (PLAN.md §6 M7). Reads on the thread pool; suggests a provider profile by
// an exact preset-endpoint match (ProviderRegistry::matchEndpoint), rclone's
// provider name, or AWS itself — the user confirms it. Saving writes each
// secret to the keychain first, then the account, one after the other.
// Temporary or indirect credentials (session tokens, SSO, roles, rclone's
// env_auth) are offered as skipped, with the reason.
//
// Thread affinity: GUI thread.
class AccountImporter : public QObject {
    Q_OBJECT

public:
    enum class Source { Aws, Rclone };

    AccountImporter(AccountStore* accounts, SecretStore* secrets, QObject* parent = nullptr);

    // The files read when the user picks none: AWS credentials + config,
    // rclone's first candidate — only those that exist and are readable
    // (a Flatpak sandbox sees none of them).
    [[nodiscard]] static QStringList defaultFiles(Source source);

    // Reads `files` (AWS: a file named like "config" is the config file,
    // any other the credentials file) and emits candidatesReady.
    void read(Source source, const QStringList& files);

    // Pure: what the files offer, against the accounts that exist.
    [[nodiscard]] static QList<ImportCandidate> fromAws(const QList<s3::AwsProfileEntry>& profiles,
                                                        const QList<Account>& existing);
    [[nodiscard]] static QList<ImportCandidate> fromRclone(const RcloneConfigReader::Result& config,
                                                           const QList<Account>& existing);
    // The account a candidate becomes (a new id).
    [[nodiscard]] static Account accountFor(const ImportCandidate& candidate, Environment environment, bool readOnly);

    // Saves the candidates one after the other; saveFinished at the end.
    void save(const QList<ImportCandidate>& chosen, Environment environment, bool readOnly);
    [[nodiscard]] bool isSaving() const { return m_saving; }

signals:
    // `problem`: why nothing could be read (translated), else empty.
    void candidatesReady(const QList<qbucket::ImportCandidate>& candidates, const QString& problem);
    void saveFinished(int saved, bool sessionOnly, const QStringList& errors);

private:
    void saveNext_();

    AccountStore* m_accounts;
    SecretStore* m_secrets;
    bool m_saving = false;
    QList<ImportCandidate> m_queue;
    Environment m_environment = Environment::Production;
    bool m_readOnly = true;
    int m_saved = 0;
    bool m_sessionOnly = false;
    QStringList m_errors;
};

} // namespace qbucket
