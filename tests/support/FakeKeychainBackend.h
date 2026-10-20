#pragma once

#include "accounts/KeychainBackend.h"

#include <QHash>
#include <QTimer>

namespace qbucket::testing {

// In-memory keychain with switchable failure modes. Answers asynchronously,
// like the real one.
class FakeKeychainBackend final : public KeychainBackend {
public:
    Status mode = Status::Ok;   // NoBackend / AccessDenied / Failed: every call fails that way
    QHash<QString, QString> entries;   // "service|key" → secret
    int reads = 0;
    int writes = 0;
    int removes = 0;

    void read(const QString& service, const QString& key, ReadCallback done) override
    {
        ++reads;
        const QString k = service + u'|' + key;
        later([this, k, done] {
            if (mode != Status::Ok)
                done(mode, {}, QStringLiteral("fake keychain: unavailable"));
            else if (!entries.contains(k))
                done(Status::NotFound, {}, QStringLiteral("fake keychain: not found"));
            else
                done(Status::Ok, entries.value(k), {});
        });
    }

    void write(const QString& service, const QString& key, const QString& secret, DoneCallback done) override
    {
        ++writes;
        const QString k = service + u'|' + key;
        later([this, k, secret, done] {
            if (mode != Status::Ok) {
                done(mode, QStringLiteral("fake keychain: unavailable"));
                return;
            }
            entries.insert(k, secret);
            done(Status::Ok, {});
        });
    }

    void remove(const QString& service, const QString& key, DoneCallback done) override
    {
        ++removes;
        const QString k = service + u'|' + key;
        later([this, k, done] {
            if (mode != Status::Ok) {
                done(mode, QStringLiteral("fake keychain: unavailable"));
                return;
            }
            entries.remove(k);
            done(Status::Ok, {});
        });
    }

private:
    template <typename F>
    static void later(F f) { QTimer::singleShot(1, std::move(f)); }
};

} // namespace qbucket::testing
