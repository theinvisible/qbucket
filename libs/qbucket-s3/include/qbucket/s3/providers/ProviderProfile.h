#pragma once

#include <qbucket/s3/Capabilities.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Operation.h>
#include <qbucket/s3/Types.h>

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace qbucket::s3 {

enum class ListApi : quint8 { V2, V1 };

// The parts of an outgoing request a provider profile may adjust before it
// is signed (ProviderProfile::prepareRequest).
struct RequestContext {
    Operation operation = Operation::ListBuckets;
    QByteArray method;
    QString bucket;
    QString key;
    QList<QPair<QByteArray, QByteArray>> headers;   // extra headers (signed if x-amz-*, content-*, range)
    AddressingStyle addressing = AddressingStyle::Path;
    bool https = true;
    // UNSIGNED-PAYLOAD instead of the body's SHA-256. Only ever honoured
    // over TLS; in-memory bodies are always hashed by default.
    bool unsignedPayload = false;
};

// Everything provider-specific, in one place (PLAN.md §3.1). The protocol
// core never checks which provider it talks to — it calls these hooks.
// GenericS3Profile holds the S3 defaults; concrete profiles override only
// what differs.
//
// Thread-safety: profiles are immutable; all members are const and may be
// called from any thread.
class QBUCKET_S3_EXPORT ProviderProfile {
public:
    virtual ~ProviderProfile();

    // --- identity ---
    [[nodiscard]] virtual QString id() const = 0;            // stable, e.g. "hetzner"
    [[nodiscard]] virtual QString displayName() const = 0;

    // --- endpoints & regions ---
    // Known regions; empty for providers with free-form endpoints.
    [[nodiscard]] virtual QStringList regions() const { return {}; }
    // Service endpoint for a region; invalid URL when the user must enter it.
    [[nodiscard]] virtual QUrl endpointFor(const QString& region) const;
    // Addressing style used for AddressingStyle::Auto.
    [[nodiscard]] virtual AddressingStyle defaultAddressing() const { return AddressingStyle::Path; }
    // Region to sign with; empty input falls back to "us-east-1".
    [[nodiscard]] virtual QString signingRegion(const QString& region) const;
    // GetBucketLocation value → region ("" → us-east-1, "EU" → eu-west-1).
    [[nodiscard]] virtual QString regionFromLocationConstraint(const QString& constraint) const;
    // CreateBucket LocationConstraint for a region; empty: send no
    // CreateBucketConfiguration (S3: us-east-1).
    [[nodiscard]] virtual QString locationConstraintFor(const QString& region) const;

    // --- capabilities ---
    // Static knowledge (source Profile) — only the fallback; definitive
    // probe results win (PLAN.md §3.3). Default: nothing known.
    [[nodiscard]] virtual CapabilitySet capabilities() const;
    // Correct a probe result the provider is known to answer misleadingly
    // (e.g. a stub read endpoint for a feature it cannot enable).
    virtual void reviewProbe(Feature feature, CapabilityEntry& entry) const;

    // --- limits ---
    [[nodiscard]] virtual qint64 minPartSize() const { return Q_INT64_C(5) * 1024 * 1024; }
    [[nodiscard]] virtual qint64 maxPartSize() const { return Q_INT64_C(5) * 1024 * 1024 * 1024; }
    [[nodiscard]] virtual int maxParts() const { return 10000; }
    [[nodiscard]] virtual int maxPresignExpirySeconds() const { return 7 * 24 * 3600; }
    [[nodiscard]] virtual int maxDeleteBatch() const { return 1000; }
    [[nodiscard]] virtual int maxKeysPerList() const { return 1000; }

    // --- storage classes ---
    // The classes an object can be put into; empty: not known (any value
    // may be tried, the server refuses unknown ones with InvalidStorageClass).
    [[nodiscard]] virtual QStringList storageClasses() const { return {}; }
    // Classes whose objects can't be read before a restore (GLACIER,
    // DEEP_ARCHIVE); a copy of such an object fails with InvalidObjectState.
    [[nodiscard]] virtual bool isArchiveStorageClass(const QString& storageClass) const;

    // --- protocol choices & quirks ---
    [[nodiscard]] virtual ListApi listApi() const { return ListApi::V2; }
    // Some servers form-encode spaces as '+' in encoding-type=url listings.
    [[nodiscard]] virtual bool listingDecodesPlusAsSpace() const { return false; }

    // Before signing: add/strip headers, force path-style, payload signing.
    virtual void prepareRequest(RequestContext& ctx) const;
    // After parsing a listing page. Default: quote unquoted ETags.
    virtual void normalizeListing(ListPage& page) const;
    // After mapping an error response (provider-specific codes, NotImplemented
    // variants, retryability).
    virtual void mapError(Error& error) const;

protected:
    ProviderProfile() = default;
    ProviderProfile(const ProviderProfile&) = default;
    ProviderProfile& operator=(const ProviderProfile&) = default;
};

} // namespace qbucket::s3
