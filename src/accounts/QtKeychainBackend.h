#pragma once

#include "accounts/KeychainBackend.h"

namespace qbucket {

// KeychainBackend on QtKeychain: Windows Credential Manager, libsecret
// (GNOME Keyring, KWallet via Secret Service, the Flatpak Secret portal) or
// KWallet. Never with QtKeychain's insecure plaintext fallback.
class QtKeychainBackend final : public KeychainBackend {
public:
    void read(const QString& service, const QString& key, ReadCallback done) override;
    void write(const QString& service, const QString& key, const QString& secret,
               DoneCallback done) override;
    void remove(const QString& service, const QString& key, DoneCallback done) override;
};

} // namespace qbucket
