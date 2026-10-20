#pragma once

#include "accounts/SecretStore.h"
#include "domain/Account.h"

#include <qbucket/s3/Credentials.h>

#include <QPointer>

#include <memory>

namespace qbucket {

// Hands an account's keychain secret to the S3 library (s3::CredentialsProvider).
// The library calls fetch() on its worker thread; the provider hops to the
// GUI thread (where SecretStore lives), reads the secret and answers from
// there — exactly once, also when the store is already gone.
//
// Create with create() on the GUI thread; destroy Sessions before the
// SecretStore (the provider then reports an error instead of crashing).
class KeychainCredentialsProvider final
    : public s3::CredentialsProvider,
      public std::enable_shared_from_this<KeychainCredentialsProvider> {
public:
    [[nodiscard]] static std::shared_ptr<KeychainCredentialsProvider> create(SecretStore* store,
                                                                             const Account& account);

    void fetch(Callback done) override;

private:
    KeychainCredentialsProvider(SecretStore* store, const Account& account);

    QPointer<SecretStore> m_store;   // read on the GUI thread only
    QUuid m_accountId;
    QString m_accessKeyId;
};

} // namespace qbucket
