#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/providers/ProviderProfile.h>

#include <QList>
#include <QString>
#include <QUrl>

#include <memory>
#include <optional>

namespace qbucket::s3 {

// Provider profiles by id. Adding a provider = one profile class + one
// line in builtin() (PLAN.md §3.1).
//
// Thread-safety: const members are thread-safe; builtin() is immutable.
// add() must not race with readers.
class QBUCKET_S3_EXPORT ProviderRegistry {
public:
    ProviderRegistry() = default;

    // hetzner, aws, garage, ceph, generic — in presentation order (Hetzner first).
    [[nodiscard]] static const ProviderRegistry& builtin();

    // Replaces a profile with the same id.
    void add(std::shared_ptr<const ProviderProfile> profile);

    // Null for an unknown id.
    [[nodiscard]] std::shared_ptr<const ProviderProfile> find(const QString& id) const;
    // GenericS3Profile for an unknown or empty id (custom endpoints).
    [[nodiscard]] std::shared_ptr<const ProviderProfile> findOrGeneric(const QString& id) const;
    [[nodiscard]] const QList<std::shared_ptr<const ProviderProfile>>& profiles() const;

    struct EndpointMatch {
        QString profileId;
        QString region;
    };
    // The profile and region whose preset endpoint (endpointFor(region) of
    // one of its regions()) `url` is — exactly: scheme and host compared
    // case-insensitively, the scheme's default port filled in, no path
    // beyond "/", no query, no user info. A suggestion for setting an
    // account up (an import), confirmed by the user; the protocol core never
    // asks it what it talks to (PLAN.md §3.1: no hostname sniffing).
    [[nodiscard]] std::optional<EndpointMatch> matchEndpoint(const QUrl& url) const;

private:
    QList<std::shared_ptr<const ProviderProfile>> m_profiles;
};

} // namespace qbucket::s3
