#pragma once

#include <QString>

#include <functional>

namespace qbucket {

// The operating system's secret storage, reduced to what SecretStore needs.
// QtKeychainBackend is the real one; tests use an in-memory fake.
//
// Thread affinity: GUI thread. Callbacks arrive asynchronously on it.
class KeychainBackend {
public:
    enum class Status {
        Ok,
        NotFound,
        NoBackend,      // no keychain on this system (e.g. no Secret Service)
        AccessDenied,   // locked keychain, user refused
        Failed,
    };

    using ReadCallback = std::function<void(Status, const QString& secret, const QString& error)>;
    using DoneCallback = std::function<void(Status, const QString& error)>;

    virtual ~KeychainBackend() = default;

    virtual void read(const QString& service, const QString& key, ReadCallback done) = 0;
    virtual void write(const QString& service, const QString& key, const QString& secret,
                       DoneCallback done) = 0;
    virtual void remove(const QString& service, const QString& key, DoneCallback done) = 0;

protected:
    KeychainBackend() = default;
    KeychainBackend(const KeychainBackend&) = default;
    KeychainBackend& operator=(const KeychainBackend&) = default;
};

} // namespace qbucket
