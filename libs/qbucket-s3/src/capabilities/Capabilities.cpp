#include <qbucket/s3/Capabilities.h>

#include <QCryptographicHash>
#include <QJsonArray>

#include <array>
#include <set>
#include <utility>

namespace qbucket::s3 {
namespace {

// Stable ids — never change one, only append.
constexpr std::array<std::pair<Feature, const char*>, 29> kIds = {{
    {Feature::BucketVersioning, "bucket.versioning"},
    {Feature::ObjectLock, "bucket.object-lock"},
    {Feature::LegalHold, "object.legal-hold"},
    {Feature::Retention, "object.retention"},
    {Feature::Lifecycle, "bucket.lifecycle"},
    {Feature::Cors, "bucket.cors"},
    {Feature::BucketPolicy, "bucket.policy"},
    {Feature::PolicyStatus, "bucket.policy-status"},
    {Feature::BucketAcl, "bucket.acl"},
    {Feature::ObjectAcl, "object.acl"},
    {Feature::BucketTagging, "bucket.tagging"},
    {Feature::ObjectTagging, "object.tagging"},
    {Feature::BucketEncryption, "bucket.encryption"},
    {Feature::SseC, "object.sse-c"},
    {Feature::Website, "bucket.website"},
    {Feature::Replication, "bucket.replication"},
    {Feature::Logging, "bucket.logging"},
    {Feature::Notifications, "bucket.notifications"},
    {Feature::PublicAccessBlock, "bucket.public-access-block"},
    {Feature::OwnershipControls, "bucket.ownership-controls"},
    {Feature::MultipartListing, "bucket.multipart-listing"},
    {Feature::ObjectVersionsListing, "bucket.versions-listing"},
    {Feature::ConditionalWrites, "object.conditional-writes"},
    {Feature::CrossBucketCopy, "object.cross-bucket-copy"},
    {Feature::BatchDelete, "bucket.batch-delete"},
    {Feature::Presign, "object.presign"},
    {Feature::MultipartUpload, "object.multipart-upload"},
    {Feature::ObjectTaggingWrite, "object.tagging-write"},
    {Feature::ObjectAclWrite, "object.acl-write"},
}};

bool isDefinitive(CapabilityState s)
{
    return s != CapabilityState::Unknown;
}

std::optional<CapabilityState> stateFromName(const QString& name)
{
    for (const CapabilityState s : {CapabilityState::Unknown, CapabilityState::Supported,
                                    CapabilityState::Unsupported, CapabilityState::NotPermitted}) {
        if (stateName(s) == name)
            return s;
    }
    return std::nullopt;
}

QString sourceName(CapabilitySource s)
{
    switch (s) {
    case CapabilitySource::Profile: return QStringLiteral("Profile");
    case CapabilitySource::Probed:  return QStringLiteral("Probed");
    case CapabilitySource::Learned: return QStringLiteral("Learned");
    }
    return QStringLiteral("Profile");
}

CapabilitySource sourceFromName(const QString& name)
{
    if (name == QLatin1String("Probed"))
        return CapabilitySource::Probed;
    if (name == QLatin1String("Learned"))
        return CapabilitySource::Learned;
    return CapabilitySource::Profile;
}

std::set<Feature> featureUnion(const CapabilitySet& a, const CapabilitySet& b)
{
    std::set<Feature> out;
    for (const auto& [f, e] : a.entries())
        out.insert(f);
    for (const auto& [f, e] : b.entries())
        out.insert(f);
    return out;
}

} // namespace

QString featureId(Feature feature)
{
    for (const auto& [f, id] : kIds) {
        if (f == feature)
            return QString::fromLatin1(id);
    }
    return QStringLiteral("unknown");
}

std::optional<Feature> featureFromId(const QString& id)
{
    for (const auto& [f, name] : kIds) {
        if (id == QLatin1String(name))
            return f;
    }
    return std::nullopt;
}

QList<Feature> allFeatures()
{
    QList<Feature> out;
    for (const auto& [f, id] : kIds)
        out.append(f);
    return out;
}

QList<Feature> featuresFor(Operation op)
{
    switch (op) {
    case Operation::DeleteObjects:
        return {Feature::BatchDelete};
    case Operation::CreateMultipartUpload:
    case Operation::UploadPart:
    case Operation::CompleteMultipartUpload:
    case Operation::AbortMultipartUpload:
    case Operation::ListParts:
    case Operation::UploadPartCopy:
        return {Feature::MultipartUpload};
    case Operation::ListMultipartUploads:
        return {Feature::MultipartListing};
    case Operation::GetObjectTagging:
        return {Feature::ObjectTagging};
    case Operation::PutObjectTagging:
        return {Feature::ObjectTaggingWrite, Feature::ObjectTagging};
    case Operation::GetObjectAcl:
        return {Feature::ObjectAcl};
    case Operation::PutObjectAcl:
        return {Feature::ObjectAclWrite, Feature::ObjectAcl};
    case Operation::ListObjectVersions:
        return {Feature::ObjectVersionsListing};
    case Operation::GetBucketVersioning:
        return {Feature::BucketVersioning};
    default:
        return {};
    }
}

QString stateName(CapabilityState state)
{
    switch (state) {
    case CapabilityState::Unknown:      return QStringLiteral("Unknown");
    case CapabilityState::Supported:    return QStringLiteral("Supported");
    case CapabilityState::Unsupported:  return QStringLiteral("Unsupported");
    case CapabilityState::NotPermitted: return QStringLiteral("NotPermitted");
    }
    return QStringLiteral("Unknown");
}

QString ServerFingerprint::hash() const
{
    const QString material = serverHeader + u'|' + amzHeaderNames.join(u',') + u'|' + requestIdShape;
    return QString::fromLatin1(
        QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

CapabilityEntry CapabilitySet::entry(Feature f) const
{
    const auto it = m_entries.find(f);
    return it == m_entries.end() ? CapabilityEntry{} : it->second;
}

QJsonObject CapabilitySet::toJson() const
{
    QJsonObject features;
    for (const auto& [f, e] : m_entries) {
        QJsonObject o;
        o.insert(QStringLiteral("state"), stateName(e.state));
        o.insert(QStringLiteral("source"), sourceName(e.source));
        if (e.checkedAt.isValid())
            o.insert(QStringLiteral("checkedAt"), e.checkedAt.toUTC().toString(Qt::ISODate));
        if (e.httpStatus != 0)
            o.insert(QStringLiteral("httpStatus"), e.httpStatus);
        if (!e.s3Code.isEmpty())
            o.insert(QStringLiteral("s3Code"), e.s3Code);
        if (e.configured)
            o.insert(QStringLiteral("configured"), true);
        if (e.unknownAttempts != 0)
            o.insert(QStringLiteral("unknownAttempts"), e.unknownAttempts);
        features.insert(featureId(f), o);
    }
    QJsonObject fp;
    fp.insert(QStringLiteral("server"), fingerprint.serverHeader);
    fp.insert(QStringLiteral("amzHeaders"), QJsonArray::fromStringList(fingerprint.amzHeaderNames));
    fp.insert(QStringLiteral("requestIdShape"), fingerprint.requestIdShape);

    QJsonObject state;
    state.insert(QStringLiteral("versioning"), bucketState.versioning);
    state.insert(QStringLiteral("objectLockEnabled"),
                 bucketState.objectLockEnabled ? QJsonValue(*bucketState.objectLockEnabled)
                                               : QJsonValue(QJsonValue::Null));

    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("scope"), scope == CapabilityScope::Bucket ? QStringLiteral("bucket")
                                                                          : QStringLiteral("server"));
    if (!bucket.isEmpty())
        root.insert(QStringLiteral("bucket"), bucket);
    if (probedAt.isValid())
        root.insert(QStringLiteral("probedAt"), probedAt.toUTC().toString(Qt::ISODate));
    root.insert(QStringLiteral("fingerprint"), fp);
    root.insert(QStringLiteral("bucketState"), state);
    root.insert(QStringLiteral("features"), features);
    return root;
}

Result<CapabilitySet> CapabilitySet::fromJson(const QJsonObject& json)
{
    if (json.value(QLatin1String("schema")).toInt() != 1)
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("unsupported capability schema"));

    CapabilitySet set;
    set.scope = json.value(QLatin1String("scope")).toString() == QLatin1String("bucket")
        ? CapabilityScope::Bucket
        : CapabilityScope::Server;
    set.bucket = json.value(QLatin1String("bucket")).toString();
    set.probedAt = QDateTime::fromString(json.value(QLatin1String("probedAt")).toString(), Qt::ISODate);

    const QJsonObject fp = json.value(QLatin1String("fingerprint")).toObject();
    set.fingerprint.serverHeader = fp.value(QLatin1String("server")).toString();
    const QJsonArray amzHeaders = fp.value(QLatin1String("amzHeaders")).toArray();
    for (const auto& v : amzHeaders)
        set.fingerprint.amzHeaderNames.append(v.toString());
    set.fingerprint.requestIdShape = fp.value(QLatin1String("requestIdShape")).toString();

    const QJsonObject state = json.value(QLatin1String("bucketState")).toObject();
    set.bucketState.versioning = state.value(QLatin1String("versioning")).toString();
    const QJsonValue lock = state.value(QLatin1String("objectLockEnabled"));
    if (lock.isBool())
        set.bucketState.objectLockEnabled = lock.toBool();

    const QJsonObject features = json.value(QLatin1String("features")).toObject();
    for (auto it = features.begin(); it != features.end(); ++it) {
        const std::optional<Feature> f = featureFromId(it.key());
        if (!f)
            continue;   // written by a newer version
        const QJsonObject o = it.value().toObject();
        CapabilityEntry e;
        e.state = stateFromName(o.value(QLatin1String("state")).toString()).value_or(CapabilityState::Unknown);
        e.source = sourceFromName(o.value(QLatin1String("source")).toString());
        e.checkedAt = QDateTime::fromString(o.value(QLatin1String("checkedAt")).toString(), Qt::ISODate);
        e.httpStatus = o.value(QLatin1String("httpStatus")).toInt();
        e.s3Code = o.value(QLatin1String("s3Code")).toString();
        e.configured = o.value(QLatin1String("configured")).toBool();
        e.unknownAttempts = o.value(QLatin1String("unknownAttempts")).toInt();
        set.set(*f, e);
    }
    return set;
}

namespace capabilities {

CapabilityEntry effective(Feature f, const CapabilitySet& probed, const CapabilitySet& profile)
{
    CapabilityEntry p = probed.entry(f);
    if (isDefinitive(p.state))
        return p;
    CapabilityEntry known = profile.entry(f);
    if (isDefinitive(known.state))
        return known;
    return p;
}

Decision decide(Feature f, const CapabilitySet& probed, const CapabilitySet& profile, bool destructive)
{
    switch (effective(f, probed, profile).state) {
    case CapabilityState::Supported:
        return Decision::Allowed;
    case CapabilityState::Unsupported:
    case CapabilityState::NotPermitted:
        return Decision::Blocked;
    case CapabilityState::Unknown:
        return destructive ? Decision::Blocked : Decision::AllowedUnverified;
    }
    return Decision::Blocked;
}

CapabilitySet merge(const CapabilitySet& previous, const CapabilitySet& fresh)
{
    // Different server software: old answers say nothing about this one.
    if (!previous.fingerprint.isEmpty() && !fresh.fingerprint.isEmpty()
        && previous.fingerprint != fresh.fingerprint) {
        return fresh;
    }

    CapabilitySet out = fresh;
    for (const Feature f : featureUnion(previous, fresh)) {
        if (!fresh.contains(f)) {
            out.set(f, previous.entry(f));
            continue;
        }
        const CapabilityEntry before = previous.entry(f);
        CapabilityEntry now = fresh.entry(f);
        if (isDefinitive(now.state)) {
            now.unknownAttempts = 0;       // downgrade and validated upgrade alike
            out.set(f, now);
        } else if (isDefinitive(before.state)) {
            out.set(f, before);            // a 5xx never erases what we knew
        } else {
            now.unknownAttempts = before.unknownAttempts + std::max(1, now.unknownAttempts);
            out.set(f, now);
        }
    }
    return out;
}

QList<CapabilityChange> diff(const CapabilitySet& before, const CapabilitySet& after)
{
    QList<CapabilityChange> out;
    for (const Feature f : featureUnion(before, after)) {
        const CapabilityState a = before.state(f);
        const CapabilityState b = after.state(f);
        if (a != b)
            out.append({f, a, b});
    }
    return out;
}

bool isExpired(const CapabilitySet& set, const QDateTime& now, const ExpiryPolicy& policy)
{
    if (!set.probedAt.isValid())
        return true;
    const int limit = set.scope == CapabilityScope::Bucket ? policy.bucketSecs : policy.serverSecs;
    if (set.probedAt.secsTo(now) >= limit)
        return true;
    if (policy.unknownBackoffSecs.isEmpty())
        return false;
    for (const auto& [f, e] : set.entries()) {
        if (e.state != CapabilityState::Unknown || e.source != CapabilitySource::Probed
            || !e.checkedAt.isValid())
            continue;
        const int index = std::clamp(e.unknownAttempts, 1, int(policy.unknownBackoffSecs.size())) - 1;
        if (e.checkedAt.secsTo(now) >= policy.unknownBackoffSecs.at(index))
            return true;
    }
    return false;
}

} // namespace capabilities
} // namespace qbucket::s3
