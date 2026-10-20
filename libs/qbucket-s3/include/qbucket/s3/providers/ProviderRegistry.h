#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/providers/ProviderProfile.h>

#include <QList>
#include <QString>

#include <memory>

namespace qbucket::s3 {

// Provider profiles by id. Adding a provider = one profile class + one
// line in builtin() (PLAN.md §3.1).
//
// Thread-safety: const members are thread-safe; builtin() is immutable.
// add() must not race with readers.
class QBUCKET_S3_EXPORT ProviderRegistry {
public:
    ProviderRegistry() = default;

    // generic, hetzner, aws, garage — in presentation order (Hetzner first).
    [[nodiscard]] static const ProviderRegistry& builtin();

    // Replaces a profile with the same id.
    void add(std::shared_ptr<const ProviderProfile> profile);

    // Null for an unknown id.
    [[nodiscard]] std::shared_ptr<const ProviderProfile> find(const QString& id) const;
    // GenericS3Profile for an unknown or empty id (custom endpoints).
    [[nodiscard]] std::shared_ptr<const ProviderProfile> findOrGeneric(const QString& id) const;
    [[nodiscard]] const QList<std::shared_ptr<const ProviderProfile>>& profiles() const;

private:
    QList<std::shared_ptr<const ProviderProfile>> m_profiles;
};

} // namespace qbucket::s3
