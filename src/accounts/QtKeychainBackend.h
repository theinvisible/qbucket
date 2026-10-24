#pragma once

#include "accounts/KeychainBackend.h"

#include <functional>

namespace qbucket {

// KeychainBackend on QtKeychain: Windows Credential Manager, libsecret
// (GNOME Keyring, KWallet via Secret Service, the Flatpak Secret portal) or
// KWallet. Never with QtKeychain's insecure plaintext fallback.
//
// QtKeychain on Linux reports a missing keyring daemon as an ordinary error,
// never as "no backend". With a `serviceCheck` (platform::
// checkKeychainService) every failure asks whether a keychain service
// exists at all; without one it becomes NoBackend — SecretStore's
// session-only mode — instead of a failure that keeps an account from being
// saved. Without a check, failures stay Failed.
class QtKeychainBackend final : public KeychainBackend {
public:
    using ServiceCheck = std::function<void(std::function<void(bool exists)>)>;

    explicit QtKeychainBackend(ServiceCheck serviceCheck = {});

    void read(const QString& service, const QString& key, ReadCallback done) override;
    void write(const QString& service, const QString& key, const QString& secret,
               DoneCallback done) override;
    void remove(const QString& service, const QString& key, DoneCallback done) override;

private:
    ServiceCheck m_serviceCheck;
};

} // namespace qbucket
