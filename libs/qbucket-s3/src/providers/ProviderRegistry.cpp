#include <qbucket/s3/providers/AwsProfile.h>
#include <qbucket/s3/providers/GarageProfile.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/providers/HetznerProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

namespace qbucket::s3 {

const ProviderRegistry& ProviderRegistry::builtin()
{
    static const ProviderRegistry registry = [] {
        ProviderRegistry r;
        r.add(std::make_shared<HetznerProfile>());
        r.add(std::make_shared<AwsProfile>());
        r.add(std::make_shared<GarageProfile>());
        r.add(std::make_shared<GenericS3Profile>());
        return r;
    }();
    return registry;
}

void ProviderRegistry::add(std::shared_ptr<const ProviderProfile> profile)
{
    Q_ASSERT(profile);
    for (auto& existing : m_profiles) {
        if (existing->id() == profile->id()) {
            existing = std::move(profile);
            return;
        }
    }
    m_profiles.append(std::move(profile));
}

std::shared_ptr<const ProviderProfile> ProviderRegistry::find(const QString& id) const
{
    for (const auto& p : m_profiles) {
        if (p->id() == id)
            return p;
    }
    return nullptr;
}

std::shared_ptr<const ProviderProfile> ProviderRegistry::findOrGeneric(const QString& id) const
{
    if (auto p = find(id))
        return p;
    if (auto generic = find(QStringLiteral("generic")))
        return generic;
    return std::make_shared<GenericS3Profile>();
}

const QList<std::shared_ptr<const ProviderProfile>>& ProviderRegistry::profiles() const
{
    return m_profiles;
}

} // namespace qbucket::s3
