#pragma once

#include <qbucket/s3/Endpoint.h>

#include <QString>
#include <QStringList>
#include <QUuid>

namespace qbucket {

// How careful the UI must be with an account (PLAN.md §12): Production is
// the default — an account has to be marked harmless explicitly.
enum class Environment : quint8 { Production, Staging, Development };

// One S3 account as the app stores it. Plain value; never holds a secret —
// the secret access key lives in the keychain (accounts/SecretStore) under
// the account id.
struct Account {
    QUuid id;
    QString name;
    QString providerId;             // ProviderRegistry id; "generic" for custom endpoints
    QString region;
    QString endpoint;               // URL; empty: the provider's endpoint for the region
    s3::AddressingStyle addressing = s3::AddressingStyle::Auto;
    QString accessKeyId;
    Environment environment = Environment::Production;
    bool readOnly = false;          // refuses every mutation (OperationController, M2)
    QStringList externalBuckets;    // buckets reachable without ListBuckets permission

    [[nodiscard]] bool isValid() const
    {
        return !id.isNull() && !name.trimmed().isEmpty() && !accessKeyId.isEmpty()
            && !providerId.isEmpty();
    }
};

// Same endpoint and identity: a session built for one serves the other.
inline bool sameConnection(const Account& a, const Account& b)
{
    return a.providerId == b.providerId && a.region == b.region && a.endpoint == b.endpoint
        && a.addressing == b.addressing && a.accessKeyId == b.accessKeyId;
}

} // namespace qbucket
