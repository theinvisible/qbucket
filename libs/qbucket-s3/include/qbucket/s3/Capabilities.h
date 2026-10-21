#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Operation.h>
#include <qbucket/s3/Result.h>

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <map>
#include <optional>

// The dynamic capability safeguard (PLAN.md §3.3): what a server can do,
// learned by read-only probes, with the provider profile's static
// knowledge as fallback. All types are values; functions are pure and
// thread-safe.
namespace qbucket::s3 {

// One S3 feature the app may use. Append-only; persisted by stable string
// id (featureId), never by number.
enum class Feature : quint16 {
    BucketVersioning,
    ObjectLock,
    LegalHold,
    Retention,
    Lifecycle,
    Cors,
    BucketPolicy,
    PolicyStatus,
    BucketAcl,
    ObjectAcl,
    BucketTagging,
    ObjectTagging,
    BucketEncryption,      // SSE-S3 / SSE-KMS default encryption
    SseC,                  // customer-provided keys (write-only, not probeable)
    Website,
    Replication,
    Logging,
    Notifications,
    PublicAccessBlock,
    OwnershipControls,
    MultipartListing,      // ListMultipartUploads
    ObjectVersionsListing, // ListObjectVersions
    ConditionalWrites,     // write-only
    CrossBucketCopy,       // write-only
    BatchDelete,           // DeleteObjects
    Presign,
    MultipartUpload,       // Create/UploadPart/Complete/Abort/ListParts, UploadPartCopy
                           // (write-only: profile knowledge, never probed)
    // Writing an object's tags / ACL (PutObjectTagging, PutObjectAcl, the
    // x-amz-tagging / x-amz-acl headers). Write-only and never probed: a
    // server answering GET ?tagging may still mishandle PUT ?tagging (a
    // proxy that drops the query turns it into a PutObject). Profile
    // knowledge or a deep probe on a scratch bucket only.
    ObjectTaggingWrite,
    ObjectAclWrite,
    // Switching a bucket's versioning (PutBucketVersioning). Write-only, as
    // above: a proxy that drops ?versioning turns the PUT into CreateBucket.
    BucketVersioningWrite,
};

QBUCKET_S3_EXPORT QString featureId(Feature feature);   // e.g. "bucket.versioning"
QBUCKET_S3_EXPORT std::optional<Feature> featureFromId(const QString& id);
QBUCKET_S3_EXPORT QList<Feature> allFeatures();

// Features an operation needs. Used for the library short-circuit and for
// learning from real calls. The first one is the feature the operation
// stands for (the one a 501 teaches), the others are prerequisites:
// PutObjectTagging → {ObjectTaggingWrite, ObjectTagging}.
QBUCKET_S3_EXPORT QList<Feature> featuresFor(Operation op);

enum class CapabilityState : quint8 {
    Unknown,        // not probeable read-only, or the probe failed (5xx, timeout)
    Supported,      // valid answer — including "supported, not configured"
    Unsupported,    // the server said no (501, 405 on a probe, ignored subresource)
    NotPermitted,   // 403: may exist, these credentials may not use it
};
QBUCKET_S3_EXPORT QString stateName(CapabilityState state);

enum class CapabilitySource : quint8 { Profile, Probed, Learned };

struct CapabilityEntry {
    CapabilityState state = CapabilityState::Unknown;
    CapabilitySource source = CapabilitySource::Profile;
    QDateTime checkedAt;          // UTC; invalid for profile knowledge
    int httpStatus = 0;           // raw answer of the probe, for the capabilities page
    QString s3Code;
    bool configured = false;      // Supported and something is set (e.g. a lifecycle rule)
    int unknownAttempts = 0;      // consecutive Unknown probes (back-off)
};

// Signs of which server software answered — a change forces a re-probe.
struct QBUCKET_S3_EXPORT ServerFingerprint {
    QString serverHeader;
    QStringList amzHeaderNames;   // sorted, lower-case
    QString requestIdShape;       // e.g. "hex16", "tx…-…" — pattern, not value

    [[nodiscard]] bool isEmpty() const { return serverHeader.isEmpty() && amzHeaderNames.isEmpty(); }
    [[nodiscard]] QString hash() const;
    bool operator==(const ServerFingerprint&) const = default;
};

// What a bucket is configured as — as important as what the server can do.
struct BucketState {
    QString versioning;                       // "Enabled", "Suspended" or "" (never enabled)
    std::optional<bool> objectLockEnabled;    // unset: unknown
    std::optional<bool> mfaDelete;            // unset: not reported
};

enum class CapabilityScope : quint8 { Server, Bucket };

class QBUCKET_S3_EXPORT CapabilitySet {
public:
    CapabilityScope scope = CapabilityScope::Server;
    QString bucket;                 // Bucket scope only
    QDateTime probedAt;             // UTC; invalid: never probed
    ServerFingerprint fingerprint;
    BucketState bucketState;

    [[nodiscard]] CapabilityEntry entry(Feature f) const;   // Unknown if absent
    [[nodiscard]] CapabilityState state(Feature f) const { return entry(f).state; }
    void set(Feature f, const CapabilityEntry& e) { m_entries[f] = e; }
    [[nodiscard]] bool contains(Feature f) const { return m_entries.count(f) != 0; }
    [[nodiscard]] const std::map<Feature, CapabilityEntry>& entries() const { return m_entries; }

    // Persistence without QSettings: the consumer stores the JSON wherever
    // it likes. Unknown feature ids are ignored (forward compatibility).
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static Result<CapabilitySet> fromJson(const QJsonObject& json);

private:
    std::map<Feature, CapabilityEntry> m_entries;
};

struct CapabilityChange {
    Feature feature;
    CapabilityState before;
    CapabilityState after;
};

// How fresh results must be (PLAN.md §3.3 re-validation).
struct ExpiryPolicy {
    int serverSecs = 24 * 3600;
    int bucketSecs = 10 * 60;
    QList<int> unknownBackoffSecs = {5 * 60, 30 * 60, 2 * 3600};
};

enum class Decision : quint8 {
    Allowed,             // definitive Supported (probe or profile)
    AllowedUnverified,   // Unknown, non-destructive: allowed, marked "not verified"
    Blocked,             // Unsupported, NotPermitted, or Unknown for a destructive op
};

namespace capabilities {

// Rules 1–2: a definitive probe result wins, else the profile's knowledge.
QBUCKET_S3_EXPORT CapabilityEntry effective(Feature f, const CapabilitySet& probed,
                                            const CapabilitySet& profile);

// Rules 3–4: Unknown never unlocks a destructive operation.
QBUCKET_S3_EXPORT Decision decide(Feature f, const CapabilitySet& probed,
                                  const CapabilitySet& profile, bool destructive);

// Applies a fresh probe to the previous one: downgrades at once, upgrades
// after a definitive answer; a fresh Unknown never overwrites an older
// definitive result (it only counts toward the back-off).
QBUCKET_S3_EXPORT CapabilitySet merge(const CapabilitySet& previous, const CapabilitySet& fresh);

// Features whose state differs.
QBUCKET_S3_EXPORT QList<CapabilityChange> diff(const CapabilitySet& before, const CapabilitySet& after);

// True when the set (or one of its Unknown entries) is due for a re-probe.
QBUCKET_S3_EXPORT bool isExpired(const CapabilitySet& set, const QDateTime& now,
                                 const ExpiryPolicy& policy = {});

} // namespace capabilities
} // namespace qbucket::s3
