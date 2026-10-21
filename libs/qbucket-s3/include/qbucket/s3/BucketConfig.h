#pragma once

#include <qbucket/s3/Capabilities.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Operation.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// A bucket's configuration documents — policy, CORS, lifecycle, object
// lock, ACL, public access block, and the ones this library only reads —
// moved as raw documents (Session::getBucketConfig / putBucketConfig /
// deleteBucketConfig), with typed views of the editable ones.
//
// The typed views are lossless or say so: a parser that meets an element
// its type doesn't hold sets `lossless = false`, and building that type
// again would drop it. Editors offer only the raw document then.
//
// Pure functions and value types; thread-safe.
namespace qbucket::s3 {

// Append-only.
enum class BucketConfig : quint8 {
    Policy,              // JSON
    PolicyStatus,        // read-only
    Cors,
    Lifecycle,
    ObjectLock,          // the default retention; object lock itself only at creation
    Acl,
    PublicAccessBlock,
    // Read-only in this library version:
    Tagging,
    Website,
    Logging,
    Replication,
    Encryption,
    Notifications,
    OwnershipControls,
};

QBUCKET_S3_EXPORT QList<BucketConfig> allBucketConfigs();
// The query subresource, e.g. "policy", "publicAccessBlock".
QBUCKET_S3_EXPORT QByteArray bucketConfigSubresource(BucketConfig kind);
// Stable English name, e.g. "PublicAccessBlock".
QBUCKET_S3_EXPORT QString bucketConfigName(BucketConfig kind);
// The feature reading it; and the write-only feature writing (PUT and
// DELETE) it, none for read-only kinds.
QBUCKET_S3_EXPORT Feature bucketConfigFeature(BucketConfig kind);
QBUCKET_S3_EXPORT std::optional<Feature> bucketConfigWriteFeature(BucketConfig kind);
QBUCKET_S3_EXPORT Operation bucketConfigGetOperation(BucketConfig kind);
// None: the library doesn't write / delete this kind.
QBUCKET_S3_EXPORT std::optional<Operation> bucketConfigPutOperation(BucketConfig kind);
QBUCKET_S3_EXPORT std::optional<Operation> bucketConfigDeleteOperation(BucketConfig kind);
// 404 codes meaning "supported, nothing set" (e.g. NoSuchCORSConfiguration).
QBUCKET_S3_EXPORT QStringList bucketConfigNotConfiguredCodes(BucketConfig kind);
// Root element of the XML document; empty for the JSON policy.
QBUCKET_S3_EXPORT QLatin1StringView bucketConfigRoot(BucketConfig kind);

// What GET ?<subresource> found.
struct BucketConfigDocument {
    BucketConfig kind = BucketConfig::Policy;
    // false: nothing is set — a "not configured" 404 (body empty), a 204,
    // or a document without content (<BucketLoggingStatus/>, {}).
    bool configured = false;
    QByteArray body;                      // as received
    QString requestId;
};

struct PutBucketConfigParams {
    QString bucket;
    BucketConfig kind = BucketConfig::Policy;
    QByteArray body;                      // the whole document; Acl: or cannedAcl instead
    QString cannedAcl;                    // Acl only: x-amz-acl, no body
};

// Semantic equality of two documents of one kind — what a read-back after
// a write is checked with. JSON is compared as values (a single string
// equals an array of one); the typed kinds as their types (a lifecycle
// rule's ID the server made up counts as equal to none sent); the others
// as XML without whitespace between elements.
QBUCKET_S3_EXPORT bool equivalentBucketConfig(BucketConfig kind, const QByteArray& sent, const QByteArray& got);

// --- CORS ----------------------------------------------------------------------

struct CorsRule {
    QString id;
    QStringList allowedMethods;           // GET, PUT, POST, DELETE, HEAD
    QStringList allowedOrigins;           // at most one "*" each
    QStringList allowedHeaders;
    QStringList exposeHeaders;
    std::optional<int> maxAgeSeconds;
    bool operator==(const CorsRule&) const = default;
};

struct CorsConfiguration {
    QList<CorsRule> rules;
    bool lossless = true;
};

QBUCKET_S3_EXPORT Result<CorsConfiguration> parseCorsConfiguration(const QByteArray& body);
// InvalidRequest for text XML 1.0 cannot carry.
QBUCKET_S3_EXPORT Result<QByteArray> buildCorsConfiguration(const CorsConfiguration& config);
// Empty when S3 would take it, otherwise what is wrong (English).
QBUCKET_S3_EXPORT QString corsProblem(const CorsConfiguration& config);

// --- Lifecycle ------------------------------------------------------------------

struct LifecycleTransition {
    std::optional<int> days;
    QDate date;                           // midnight UTC; instead of days
    QString storageClass;
    bool operator==(const LifecycleTransition&) const = default;
};

struct LifecycleRule {
    QString id;
    bool enabled = true;
    // The filter: a prefix, tags, or both (<And>). Empty: every object.
    QString prefix;
    TagSet tags;
    // <Prefix> directly in <Rule> (documents from before <Filter>); kept so.
    bool legacyPrefix = false;
    // Current versions.
    std::optional<int> expirationDays;
    QDate expirationDate;                 // midnight UTC; instead of days
    bool expiredObjectDeleteMarker = false;
    QList<LifecycleTransition> transitions;
    // Earlier versions (versioned buckets).
    std::optional<int> noncurrentDays;
    std::optional<int> newerNoncurrentVersions;
    // Unfinished multipart uploads.
    std::optional<int> abortIncompleteDays;
    bool operator==(const LifecycleRule&) const = default;
};

struct LifecycleConfiguration {
    QList<LifecycleRule> rules;
    bool lossless = true;
};

QBUCKET_S3_EXPORT Result<LifecycleConfiguration> parseLifecycleConfiguration(const QByteArray& body);
QBUCKET_S3_EXPORT Result<QByteArray> buildLifecycleConfiguration(const LifecycleConfiguration& config);
QBUCKET_S3_EXPORT QString lifecycleProblem(const LifecycleConfiguration& config);

// --- Object lock --------------------------------------------------------------------

struct ObjectLockConfiguration {
    bool enabled = false;                 // <ObjectLockEnabled>Enabled</…>
    // The default retention of new versions; none without a mode. Days or
    // years.
    std::optional<ObjectLockMode> mode;
    std::optional<int> days;
    std::optional<int> years;
    bool lossless = true;
    bool operator==(const ObjectLockConfiguration&) const = default;
};

QBUCKET_S3_EXPORT Result<ObjectLockConfiguration> parseObjectLockConfiguration(const QByteArray& body);
QBUCKET_S3_EXPORT QByteArray buildObjectLockConfiguration(const ObjectLockConfiguration& config);
QBUCKET_S3_EXPORT QString objectLockProblem(const ObjectLockConfiguration& config);

// --- Access ---------------------------------------------------------------------------

struct PublicAccessBlockConfiguration {
    bool blockPublicAcls = false;
    bool ignorePublicAcls = false;
    bool blockPublicPolicy = false;
    bool restrictPublicBuckets = false;
    bool lossless = true;
    bool operator==(const PublicAccessBlockConfiguration&) const = default;
};

QBUCKET_S3_EXPORT Result<PublicAccessBlockConfiguration> parsePublicAccessBlock(const QByteArray& body);
QBUCKET_S3_EXPORT QByteArray buildPublicAccessBlock(const PublicAccessBlockConfiguration& config);

// <PolicyStatus><IsPublic>…: whether the policy makes the bucket public.
QBUCKET_S3_EXPORT Result<bool> parsePolicyStatus(const QByteArray& body);

// <AccessControlPolicy> of a bucket (or object).
QBUCKET_S3_EXPORT Result<AccessControlPolicy> parseAccessControlPolicyDocument(const QByteArray& body);

// --- Policy ------------------------------------------------------------------------------

// What a policy does that deserves a warning.
struct PolicyFindings {
    bool publicRead = false;              // Allow for Principal "*" (or {"AWS": "*"})
    bool publicWrite = false;             // … with a write or delete action, or s3:*
    bool deny = false;                    // any Deny: may lock out these credentials too
};

// Empty when the document looks like a policy S3 would take (a JSON object
// with Statement entries that have Effect, Action and Resource), otherwise
// what is wrong (English). The server has the last word.
QBUCKET_S3_EXPORT QStringList policyProblems(const QByteArray& json);
QBUCKET_S3_EXPORT PolicyFindings analyzePolicy(const QByteArray& json);

// --- Bucket names -------------------------------------------------------------------------

enum class BucketNameProblem : quint8 {
    None,
    Length,              // 3 to 63 characters
    Characters,          // lower-case letters, digits, '.', '-'
    StartOrEnd,          // must start and end with a letter or digit
    AdjacentDots,        // ".."
    DotNextToHyphen,     // ".-" / "-." — not a DNS name
    IpAddress,           // like 192.168.5.4
    ReservedPrefix,      // xn--, sthree-, amzn-s3-demo-
    ReservedSuffix,      // -s3alias, --ol-s3, .mrap, --x-s3, --table-s3
};

// S3's rules for new bucket names (general purpose buckets).
QBUCKET_S3_EXPORT BucketNameProblem bucketNameProblem(const QString& name);
QBUCKET_S3_EXPORT QString bucketNameProblemText(BucketNameProblem problem);   // English

} // namespace qbucket::s3
