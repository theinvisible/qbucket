#pragma once

#include "domain/Account.h"

#include <QList>
#include <QObject>
#include <QUuid>

#include <optional>

class QSettings;

namespace qbucket {

// The non-secret account data, in its own INI file (accounts.ini under
// AppConfigLocation) — one group per account, keyed by its UUID, so deleting
// one never renumbers the others. Secrets go to SecretStore.
//
// Writes are synchronous disk I/O; accounts change rarely.
// Thread affinity: GUI thread.
class AccountStore : public QObject {
    Q_OBJECT

public:
    explicit AccountStore(QObject* parent = nullptr);
    // Explicit file, for tests.
    explicit AccountStore(const QString& filePath, QObject* parent = nullptr);
    ~AccountStore() override;

    // Sorted by name.
    [[nodiscard]] QList<Account> accounts() const { return m_accounts; }
    [[nodiscard]] std::optional<Account> account(const QUuid& id) const;

    // Insert or update; false (and nothing written) for an invalid account.
    bool save(const Account& account);
    bool remove(const QUuid& id);

    [[nodiscard]] QString fileName() const;

signals:
    void accountsChanged();

private:
    void load_();
    void sort_();

    QSettings* m_store;
    QList<Account> m_accounts;
};

} // namespace qbucket
