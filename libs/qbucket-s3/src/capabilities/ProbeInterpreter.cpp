#include "capabilities/ProbeInterpreter.h"

#include "xml/XmlParsers.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace qbucket::s3 {
namespace {

ProbeSpec bucket(Feature f, const char* sub, const char* root, QStringList notConfigured = {})
{
    return ProbeSpec{f, {{sub, {}, true}}, QLatin1StringView(root), false, std::move(notConfigured)};
}

ProbeSpec object(Feature f, const char* sub, const char* root, QStringList notConfigured = {})
{
    return ProbeSpec{f, {{sub, {}, true}}, QLatin1StringView(root), true, std::move(notConfigured)};
}

// Text of the first element with this local name; null if absent.
QString firstText(const QByteArray& body, QLatin1StringView name)
{
    QXmlStreamReader r(body);
    while (!r.atEnd()) {
        if (r.readNext() == QXmlStreamReader::StartElement && r.name() == name)
            return r.readElementText();
    }
    return {};
}

// Does the root element have any child element?
bool hasChildElements(const QByteArray& body)
{
    QXmlStreamReader r(body);
    int depth = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && ++depth == 2)
            return true;
        if (r.isEndElement())
            --depth;
    }
    return false;
}

} // namespace

const QList<ProbeSpec>& bucketProbes()
{
    static const QList<ProbeSpec> probes = {
        bucket(Feature::BucketVersioning, "versioning", "VersioningConfiguration"),
        bucket(Feature::ObjectLock, "object-lock", "ObjectLockConfiguration",
               {QStringLiteral("ObjectLockConfigurationNotFoundError"),
                QStringLiteral("NoSuchObjectLockConfiguration")}),
        bucket(Feature::Lifecycle, "lifecycle", "LifecycleConfiguration",
               {QStringLiteral("NoSuchLifecycleConfiguration")}),
        bucket(Feature::Cors, "cors", "CORSConfiguration", {QStringLiteral("NoSuchCORSConfiguration")}),
        bucket(Feature::BucketPolicy, "policy", "", {QStringLiteral("NoSuchBucketPolicy")}),
        bucket(Feature::PolicyStatus, "policyStatus", "PolicyStatus", {QStringLiteral("NoSuchBucketPolicy")}),
        bucket(Feature::BucketAcl, "acl", "AccessControlPolicy"),
        bucket(Feature::BucketTagging, "tagging", "Tagging",
               {QStringLiteral("NoSuchTagSet"), QStringLiteral("NoSuchTagSetError")}),
        bucket(Feature::BucketEncryption, "encryption", "ServerSideEncryptionConfiguration",
               {QStringLiteral("ServerSideEncryptionConfigurationNotFoundError")}),
        bucket(Feature::Website, "website", "WebsiteConfiguration",
               {QStringLiteral("NoSuchWebsiteConfiguration")}),
        bucket(Feature::Replication, "replication", "ReplicationConfiguration",
               {QStringLiteral("ReplicationConfigurationNotFoundError")}),
        bucket(Feature::Logging, "logging", "BucketLoggingStatus"),
        bucket(Feature::Notifications, "notification", "NotificationConfiguration"),
        bucket(Feature::PublicAccessBlock, "publicAccessBlock", "PublicAccessBlockConfiguration",
               {QStringLiteral("NoSuchPublicAccessBlockConfiguration")}),
        bucket(Feature::OwnershipControls, "ownershipControls", "OwnershipControls",
               {QStringLiteral("OwnershipControlsNotFoundError")}),
        bucket(Feature::MultipartListing, "uploads", "ListMultipartUploadsResult"),
        ProbeSpec{Feature::ObjectVersionsListing,
                  {{"versions", {}, true}, {"max-keys", "1", false}},
                  QLatin1StringView("ListVersionsResult"), false, {}},
    };
    return probes;
}

const QList<ProbeSpec>& objectProbes()
{
    static const QList<ProbeSpec> probes = {
        object(Feature::ObjectTagging, "tagging", "Tagging"),
        object(Feature::ObjectAcl, "acl", "AccessControlPolicy"),
        object(Feature::Retention, "retention", "Retention",
               {QStringLiteral("NoSuchObjectLockConfiguration"),
                QStringLiteral("ObjectLockConfigurationNotFoundError")}),
        object(Feature::LegalHold, "legal-hold", "LegalHold",
               {QStringLiteral("NoSuchObjectLockConfiguration"),
                QStringLiteral("ObjectLockConfigurationNotFoundError")}),
    };
    return probes;
}

CapabilityEntry interpretProbe(const ProbeSpec& spec, const Result<RawResponse>& response,
                               const QDateTime& now, BucketState* state)
{
    CapabilityEntry e;
    e.source = CapabilitySource::Probed;
    e.checkedAt = now;

    if (!response) {
        const Error& err = response.error();
        e.httpStatus = err.httpStatus;
        e.s3Code = err.code;
        if (spec.notConfiguredCodes.contains(err.code)
            || (err.category == ErrorCategory::NotConfigured && err.httpStatus == 404)) {
            e.state = CapabilityState::Supported;
            if (spec.feature == Feature::ObjectLock && state)
                state->objectLockEnabled = false;
        } else if (err.category == ErrorCategory::Unsupported || err.httpStatus == 405) {
            e.state = CapabilityState::Unsupported;
        } else if (err.category == ErrorCategory::NotPermitted) {
            e.state = CapabilityState::NotPermitted;
        } else {
            // 5xx, timeouts, cancellation, missing bucket/key, odd 4xx:
            // no statement about the feature — never Unsupported.
            e.state = CapabilityState::Unknown;
            e.unknownAttempts = 1;
        }
        return e;
    }

    const RawResponse& r = response.value();
    e.httpStatus = r.status;

    // An explicit "nothing here" (Garage for an unset website). A server
    // ignoring the subresource would send the listing instead, not 204.
    if (r.status == 204 && r.body.isEmpty()) {
        e.state = CapabilityState::Supported;
        e.s3Code = QStringLiteral("NoContent");
        return e;
    }

    if (spec.expectedRoot.isEmpty()) {   // bucket policy: a JSON document
        const QJsonDocument json = QJsonDocument::fromJson(r.body);
        e.state = json.isObject() ? CapabilityState::Supported : CapabilityState::Unsupported;
        e.configured = json.isObject() && !json.object().isEmpty();
        return e;
    }

    const QString root = xml::rootElementName(r.body);
    if (root != spec.expectedRoot) {
        // Typically a plain listing: the server ignored the subresource.
        e.state = CapabilityState::Unsupported;
        e.s3Code = root.isEmpty() ? QStringLiteral("NoXml") : QStringLiteral("Root:") + root;
        return e;
    }

    e.state = CapabilityState::Supported;
    e.configured = hasChildElements(r.body);
    if (spec.feature == Feature::BucketVersioning) {
        const QString status = firstText(r.body, QLatin1StringView("Status"));
        e.configured = !status.isEmpty();
        if (state)
            state->versioning = status;
    } else if (spec.feature == Feature::ObjectLock) {
        const bool enabled = firstText(r.body, QLatin1StringView("ObjectLockEnabled")) == QLatin1String("Enabled");
        e.configured = enabled;
        if (state)
            state->objectLockEnabled = enabled;
    } else if (spec.feature == Feature::Replication) {
        e.configured = !firstText(r.body, QLatin1StringView("Rule")).isNull();   // RGW: empty <Role/>
    } else if (spec.feature == Feature::MultipartListing || spec.feature == Feature::ObjectVersionsListing
               || spec.feature == Feature::BucketAcl || spec.feature == Feature::ObjectAcl
               || spec.feature == Feature::PolicyStatus) {
        e.configured = false;   // listings, ACLs and status documents always have content
    }
    return e;
}

ServerFingerprint fingerprintOf(const RawResponse& response)
{
    ServerFingerprint fp;
    fp.serverHeader = QString::fromLatin1(response.header("server"));
    for (const auto& [name, value] : response.headers) {
        const QByteArray lower = name.toLower();
        if (lower.startsWith("x-amz-") && !fp.amzHeaderNames.contains(QString::fromLatin1(lower)))
            fp.amzHeaderNames.append(QString::fromLatin1(lower));
    }
    fp.amzHeaderNames.sort();

    // The shape of a request id says which software produced it.
    const QString id = QString::fromLatin1(response.header("x-amz-request-id"));
    static const QRegularExpression rgw(QStringLiteral("^tx[0-9a-f]+-[0-9a-f]+-"));
    static const QRegularExpression hex(QStringLiteral("^[0-9A-Fa-f]+$"));
    if (id.isEmpty())
        fp.requestIdShape = QStringLiteral("none");
    else if (rgw.match(id).hasMatch())
        fp.requestIdShape = QStringLiteral("rgw-tx");
    else if (hex.match(id).hasMatch())
        fp.requestIdShape = QStringLiteral("hex%1").arg(id.size());
    else
        fp.requestIdShape = QStringLiteral("other%1").arg(id.size());
    return fp;
}

} // namespace qbucket::s3
