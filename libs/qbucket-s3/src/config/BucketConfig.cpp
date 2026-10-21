#include <qbucket/s3/BucketConfig.h>

#include "xml/XmlParsers.h"
#include "xml/XmlWriters.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <array>

namespace qbucket::s3 {
namespace {

struct KindInfo {
    BucketConfig kind;
    const char* subresource;
    const char* name;
    const char* root;   // "" for JSON
    Feature feature;
    std::optional<Feature> writeFeature;
    Operation get;
    std::optional<Operation> put;
    std::optional<Operation> del;
    QStringList notConfigured;
};

const std::array<KindInfo, 14>& kinds()
{
    static const std::array<KindInfo, 14> table = {{
        {BucketConfig::Policy, "policy", "Policy", "", Feature::BucketPolicy, Feature::BucketPolicyWrite,
         Operation::GetBucketPolicy, Operation::PutBucketPolicy, Operation::DeleteBucketPolicy,
         {QStringLiteral("NoSuchBucketPolicy")}},
        {BucketConfig::PolicyStatus, "policyStatus", "PolicyStatus", "PolicyStatus", Feature::PolicyStatus,
         std::nullopt, Operation::GetBucketPolicyStatus, std::nullopt, std::nullopt,
         {QStringLiteral("NoSuchBucketPolicy")}},
        {BucketConfig::Cors, "cors", "Cors", "CORSConfiguration", Feature::Cors, Feature::CorsWrite,
         Operation::GetBucketCors, Operation::PutBucketCors, Operation::DeleteBucketCors,
         {QStringLiteral("NoSuchCORSConfiguration")}},
        {BucketConfig::Lifecycle, "lifecycle", "Lifecycle", "LifecycleConfiguration", Feature::Lifecycle,
         Feature::LifecycleWrite, Operation::GetBucketLifecycle, Operation::PutBucketLifecycle,
         Operation::DeleteBucketLifecycle, {QStringLiteral("NoSuchLifecycleConfiguration")}},
        {BucketConfig::ObjectLock, "object-lock", "ObjectLock", "ObjectLockConfiguration", Feature::ObjectLock,
         Feature::ObjectLockWrite, Operation::GetObjectLockConfiguration, Operation::PutObjectLockConfiguration,
         std::nullopt,
         {QStringLiteral("ObjectLockConfigurationNotFoundError"),
          QStringLiteral("NoSuchObjectLockConfiguration")}},
        {BucketConfig::Acl, "acl", "Acl", "AccessControlPolicy", Feature::BucketAcl, Feature::BucketAclWrite,
         Operation::GetBucketAcl, Operation::PutBucketAcl, std::nullopt, {}},
        {BucketConfig::PublicAccessBlock, "publicAccessBlock", "PublicAccessBlock",
         "PublicAccessBlockConfiguration", Feature::PublicAccessBlock, Feature::PublicAccessBlockWrite,
         Operation::GetPublicAccessBlock, Operation::PutPublicAccessBlock, Operation::DeletePublicAccessBlock,
         {QStringLiteral("NoSuchPublicAccessBlockConfiguration")}},
        {BucketConfig::Tagging, "tagging", "Tagging", "Tagging", Feature::BucketTagging, std::nullopt,
         Operation::GetBucketTagging, std::nullopt, std::nullopt,
         {QStringLiteral("NoSuchTagSet"), QStringLiteral("NoSuchTagSetError")}},
        {BucketConfig::Website, "website", "Website", "WebsiteConfiguration", Feature::Website, std::nullopt,
         Operation::GetBucketWebsite, std::nullopt, std::nullopt, {QStringLiteral("NoSuchWebsiteConfiguration")}},
        {BucketConfig::Logging, "logging", "Logging", "BucketLoggingStatus", Feature::Logging, std::nullopt,
         Operation::GetBucketLogging, std::nullopt, std::nullopt, {}},
        {BucketConfig::Replication, "replication", "Replication", "ReplicationConfiguration",
         Feature::Replication, std::nullopt, Operation::GetBucketReplication, std::nullopt, std::nullopt,
         {QStringLiteral("ReplicationConfigurationNotFoundError")}},
        {BucketConfig::Encryption, "encryption", "Encryption", "ServerSideEncryptionConfiguration",
         Feature::BucketEncryption, std::nullopt, Operation::GetBucketEncryption, std::nullopt, std::nullopt,
         {QStringLiteral("ServerSideEncryptionConfigurationNotFoundError")}},
        {BucketConfig::Notifications, "notification", "Notifications", "NotificationConfiguration",
         Feature::Notifications, std::nullopt, Operation::GetBucketNotification, std::nullopt, std::nullopt, {}},
        {BucketConfig::OwnershipControls, "ownershipControls", "OwnershipControls", "OwnershipControls",
         Feature::OwnershipControls, std::nullopt, Operation::GetBucketOwnershipControls, std::nullopt,
         std::nullopt, {QStringLiteral("OwnershipControlsNotFoundError")}},
    }};
    return table;
}

const KindInfo& info(BucketConfig kind)
{
    for (const KindInfo& k : kinds()) {
        if (k.kind == kind)
            return k;
    }
    return kinds().front();
}

Error protocolError(const QString& message)
{
    return Error::make(ErrorCategory::Protocol, message);
}

Error invalid(const QString& message)
{
    return Error::make(ErrorCategory::InvalidRequest, message);
}

constexpr const char* kXmlHeader = R"(<?xml version="1.0" encoding="UTF-8"?>)";
constexpr const char* kNs = R"( xmlns="http://s3.amazonaws.com/doc/2006-03-01/")";

QByteArray element(const char* name, const QString& text)
{
    return QByteArray("<") + name + ">" + xml::escapeText(text) + "</" + name + ">";
}

QByteArray element(const char* name, int value)
{
    return element(name, QString::number(value));
}

// Text XML 1.0 can carry (no control characters but tab, LF, CR).
bool representable(const QString& text)
{
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != 0x09 && u != 0x0A && u != 0x0D) || u == 0xFFFE || u == 0xFFFF)
            return false;
    }
    return true;
}

bool representable(const QStringList& list)
{
    return std::all_of(list.begin(), list.end(), [](const QString& s) { return representable(s); });
}

// Positions the reader on the root element.
std::optional<Error> openRoot(QXmlStreamReader& r, QLatin1StringView root)
{
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (r.name() != root) {
                return protocolError(QStringLiteral("unexpected XML root element <%1>, expected <%2>")
                                         .arg(r.name().toString(), QString(root)));
            }
            return std::nullopt;
        }
    }
    return protocolError(r.hasError() ? QStringLiteral("malformed XML: %1").arg(r.errorString())
                                      : QStringLiteral("empty XML document, expected <%1>").arg(QString(root)));
}

std::optional<Error> finish(QXmlStreamReader& r)
{
    while (!r.atEnd())
        r.readNext();
    if (r.hasError())
        return protocolError(QStringLiteral("malformed XML: %1").arg(r.errorString()));
    return std::nullopt;
}

// An integer element; a non-number is no number (and not lossless).
std::optional<int> readInt(QXmlStreamReader& r, bool* lossless)
{
    bool ok = false;
    const int v = r.readElementText().trimmed().toInt(&ok);
    if (!ok) {
        *lossless = false;
        return std::nullopt;
    }
    return v;
}

// S3 lifecycle dates are midnight UTC ("2026-01-01T00:00:00.000Z"); also
// taken as a plain date.
QDate readDate(QXmlStreamReader& r, bool* lossless)
{
    const QString text = r.readElementText().trimmed();
    QDateTime t = xml::parseIsoTimestamp(text);
    if (!t.isValid())
        t = QDateTime::fromString(text, Qt::ISODate);
    if (t.isValid()) {
        t = t.toUTC();
        if (t.time() != QTime(0, 0))
            *lossless = false;
        return t.date();
    }
    const QDate d = QDate::fromString(text, Qt::ISODate);
    if (!d.isValid())
        *lossless = false;
    return d;
}

QString dateText(const QDate& d)
{
    return d.toString(Qt::ISODate) + QStringLiteral("T00:00:00.000Z");
}

bool readBool(QXmlStreamReader& r, bool* lossless)
{
    const QString text = r.readElementText().trimmed();
    if (text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
        return true;
    if (text.compare(QLatin1String("false"), Qt::CaseInsensitive) != 0)
        *lossless = false;
    return false;
}

// <Tag><Key/><Value/></Tag>
Tag readTag(QXmlStreamReader& r, bool* lossless)
{
    Tag t;
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("Key"))
            t.key = r.readElementText();
        else if (r.name() == QLatin1String("Value"))
            t.value = r.readElementText();
        else {
            *lossless = false;
            r.skipCurrentElement();
        }
    }
    return t;
}

// --- comparing ------------------------------------------------------------------

// JSON with single values and arrays of one taken as the same, objects by
// key (QJsonObject is ordered already).
QJsonValue normalizedJson(const QJsonValue& v)
{
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 1)
            return normalizedJson(a.first());
        QJsonArray out;
        for (const auto& e : a)
            out.append(normalizedJson(e));
        return out;
    }
    if (v.isObject()) {
        QJsonObject out;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            out.insert(it.key(), normalizedJson(it.value()));
        return out;
    }
    return v;
}

// The document as a sequence of elements and non-blank texts.
QStringList xmlTokens(const QByteArray& body, bool* ok)
{
    QStringList out;
    QXmlStreamReader r(body);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            out.append(QLatin1Char('<') + r.name().toString());
            for (const QXmlStreamAttribute& a : r.attributes()) {
                if (a.prefix().isEmpty() && a.name() == QLatin1String("xmlns"))
                    continue;
                out.append(QLatin1Char('@') + a.name().toString() + QLatin1Char('=') + a.value().toString());
            }
        } else if (r.isEndElement()) {
            out.append(QStringLiteral(">"));
        } else if (r.isCharacters() && !r.isWhitespace()) {
            out.append(QLatin1Char('"') + r.text().toString().trimmed());
        }
    }
    *ok = !r.hasError();
    return out;
}

bool startsWithAny(const QString& s, std::initializer_list<const char*> prefixes)
{
    return std::any_of(prefixes.begin(), prefixes.end(),
                       [&s](const char* p) { return s.startsWith(QLatin1String(p)); });
}

bool endsWithAny(const QString& s, std::initializer_list<const char*> suffixes)
{
    return std::any_of(suffixes.begin(), suffixes.end(),
                       [&s](const char* p) { return s.endsWith(QLatin1String(p)); });
}

QStringList jsonStrings(const QJsonValue& v)
{
    QStringList out;
    if (v.isString())
        out.append(v.toString());
    for (const auto& e : v.toArray()) {
        if (e.isString())
            out.append(e.toString());
    }
    return out;
}

bool publicPrincipal(const QJsonValue& principal)
{
    if (principal.isString())
        return principal.toString() == QLatin1String("*");
    if (principal.isObject()) {
        const QJsonObject o = principal.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (jsonStrings(it.value()).contains(QStringLiteral("*")))
                return true;
        }
    }
    return false;
}

QList<QJsonObject> statements(const QJsonObject& policy)
{
    QList<QJsonObject> out;
    const QJsonValue s = policy.value(QLatin1String("Statement"));
    if (s.isObject())
        out.append(s.toObject());
    for (const auto& e : s.toArray()) {
        if (e.isObject())
            out.append(e.toObject());
    }
    return out;
}

} // namespace

// --- the kinds ----------------------------------------------------------------------

QList<BucketConfig> allBucketConfigs()
{
    QList<BucketConfig> out;
    for (const KindInfo& k : kinds())
        out.append(k.kind);
    return out;
}

QByteArray bucketConfigSubresource(BucketConfig kind)
{
    return info(kind).subresource;
}

QString bucketConfigName(BucketConfig kind)
{
    return QString::fromLatin1(info(kind).name);
}

Feature bucketConfigFeature(BucketConfig kind)
{
    return info(kind).feature;
}

std::optional<Feature> bucketConfigWriteFeature(BucketConfig kind)
{
    return info(kind).writeFeature;
}

Operation bucketConfigGetOperation(BucketConfig kind)
{
    return info(kind).get;
}

std::optional<Operation> bucketConfigPutOperation(BucketConfig kind)
{
    return info(kind).put;
}

std::optional<Operation> bucketConfigDeleteOperation(BucketConfig kind)
{
    return info(kind).del;
}

QStringList bucketConfigNotConfiguredCodes(BucketConfig kind)
{
    return info(kind).notConfigured;
}

QLatin1StringView bucketConfigRoot(BucketConfig kind)
{
    return QLatin1StringView(info(kind).root);
}

bool equivalentBucketConfig(BucketConfig kind, const QByteArray& sent, const QByteArray& got)
{
    switch (kind) {
    case BucketConfig::Policy: {
        const QJsonDocument a = QJsonDocument::fromJson(sent);
        const QJsonDocument b = QJsonDocument::fromJson(got);
        if (!a.isObject() || !b.isObject())
            return false;
        return normalizedJson(a.object()) == normalizedJson(b.object());
    }
    case BucketConfig::Cors: {
        const Result<CorsConfiguration> a = parseCorsConfiguration(sent);
        const Result<CorsConfiguration> b = parseCorsConfiguration(got);
        return a && b && a->rules == b->rules;
    }
    case BucketConfig::Lifecycle: {
        const Result<LifecycleConfiguration> a = parseLifecycleConfiguration(sent);
        Result<LifecycleConfiguration> b = parseLifecycleConfiguration(got);
        if (!a || !b || a->rules.size() != b->rules.size())
            return false;
        for (qsizetype i = 0; i < a->rules.size(); ++i) {
            LifecycleRule mine = a->rules[i];
            LifecycleRule theirs = b->rules[i];
            if (mine.id.isEmpty())
                theirs.id.clear();   // made up by the server
            // A rule with a filter written as <Filter><Prefix/> or as the
            // legacy form reads back in either.
            mine.legacyPrefix = theirs.legacyPrefix = false;
            if (!(mine == theirs))
                return false;
        }
        return true;
    }
    case BucketConfig::ObjectLock: {
        Result<ObjectLockConfiguration> a = parseObjectLockConfiguration(sent);
        Result<ObjectLockConfiguration> b = parseObjectLockConfiguration(got);
        if (!a || !b)
            return false;
        a->lossless = b->lossless = true;
        return *a == *b;
    }
    case BucketConfig::PublicAccessBlock: {
        Result<PublicAccessBlockConfiguration> a = parsePublicAccessBlock(sent);
        Result<PublicAccessBlockConfiguration> b = parsePublicAccessBlock(got);
        if (!a || !b)
            return false;
        a->lossless = b->lossless = true;
        return *a == *b;
    }
    default: {
        bool okA = false;
        bool okB = false;
        const QStringList a = xmlTokens(sent, &okA);
        const QStringList b = xmlTokens(got, &okB);
        return okA && okB && a == b;
    }
    }
}

// --- CORS -------------------------------------------------------------------------------

Result<CorsConfiguration> parseCorsConfiguration(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("CORSConfiguration")))
        return *err;
    CorsConfiguration out;
    while (r.readNextStartElement()) {
        if (r.name() != QLatin1String("CORSRule")) {
            out.lossless = false;
            r.skipCurrentElement();
            continue;
        }
        CorsRule rule;
        while (r.readNextStartElement()) {
            const auto n = r.name();
            if (n == QLatin1String("ID"))
                rule.id = r.readElementText();
            else if (n == QLatin1String("AllowedMethod"))
                rule.allowedMethods.append(r.readElementText().trimmed());
            else if (n == QLatin1String("AllowedOrigin"))
                rule.allowedOrigins.append(r.readElementText().trimmed());
            else if (n == QLatin1String("AllowedHeader"))
                rule.allowedHeaders.append(r.readElementText().trimmed());
            else if (n == QLatin1String("ExposeHeader"))
                rule.exposeHeaders.append(r.readElementText().trimmed());
            else if (n == QLatin1String("MaxAgeSeconds"))
                rule.maxAgeSeconds = readInt(r, &out.lossless);
            else {
                out.lossless = false;
                r.skipCurrentElement();
            }
        }
        out.rules.append(rule);
    }
    if (auto err = finish(r))
        return *err;
    return out;
}

Result<QByteArray> buildCorsConfiguration(const CorsConfiguration& config)
{
    QByteArray out = QByteArray(kXmlHeader) + "<CORSConfiguration" + kNs + ">";
    for (const CorsRule& rule : config.rules) {
        if (!representable(rule.id) || !representable(rule.allowedMethods) || !representable(rule.allowedOrigins)
            || !representable(rule.allowedHeaders) || !representable(rule.exposeHeaders))
            return invalid(QStringLiteral("a CORS rule contains characters that cannot be sent"));
        out += "<CORSRule>";
        if (!rule.id.isEmpty())
            out += element("ID", rule.id);
        for (const QString& m : rule.allowedMethods)
            out += element("AllowedMethod", m);
        for (const QString& o : rule.allowedOrigins)
            out += element("AllowedOrigin", o);
        for (const QString& h : rule.allowedHeaders)
            out += element("AllowedHeader", h);
        for (const QString& h : rule.exposeHeaders)
            out += element("ExposeHeader", h);
        if (rule.maxAgeSeconds)
            out += element("MaxAgeSeconds", *rule.maxAgeSeconds);
        out += "</CORSRule>";
    }
    out += "</CORSConfiguration>";
    return out;
}

QString corsProblem(const CorsConfiguration& config)
{
    static const QStringList methods = {QStringLiteral("GET"), QStringLiteral("PUT"), QStringLiteral("POST"),
                                        QStringLiteral("DELETE"), QStringLiteral("HEAD")};
    if (config.rules.isEmpty())
        return QStringLiteral("a CORS configuration needs at least one rule");
    if (config.rules.size() > 100)
        return QStringLiteral("a CORS configuration has at most 100 rules");
    for (const CorsRule& rule : config.rules) {
        if (rule.id.size() > 255)
            return QStringLiteral("a rule ID has at most 255 characters");
        if (rule.allowedMethods.isEmpty())
            return QStringLiteral("every CORS rule needs an allowed method");
        for (const QString& m : rule.allowedMethods) {
            if (!methods.contains(m))
                return QStringLiteral("unknown method \"%1\" (GET, PUT, POST, DELETE, HEAD)").arg(m);
        }
        if (rule.allowedOrigins.isEmpty())
            return QStringLiteral("every CORS rule needs an allowed origin");
        for (const QString& o : rule.allowedOrigins) {
            if (o.isEmpty() || o.count(QLatin1Char('*')) > 1)
                return QStringLiteral("an origin is not empty and has at most one \"*\": \"%1\"").arg(o);
        }
        for (const QString& h : rule.allowedHeaders) {
            if (h.isEmpty() || h.count(QLatin1Char('*')) > 1)
                return QStringLiteral("an allowed header is not empty and has at most one \"*\": \"%1\"").arg(h);
        }
        for (const QString& h : rule.exposeHeaders) {
            if (h.isEmpty() || h.contains(QLatin1Char('*')))
                return QStringLiteral("an exposed header is a plain header name: \"%1\"").arg(h);
        }
        if (rule.maxAgeSeconds && *rule.maxAgeSeconds < 0)
            return QStringLiteral("the max age is not negative");
    }
    return {};
}

// --- Lifecycle ---------------------------------------------------------------------

namespace {

void readFilter(QXmlStreamReader& r, LifecycleRule* rule, bool* lossless)
{
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("Prefix")) {
            rule->prefix = r.readElementText();
        } else if (n == QLatin1String("Tag")) {
            rule->tags.append(readTag(r, lossless));
        } else if (n == QLatin1String("And")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("Prefix"))
                    rule->prefix = r.readElementText();
                else if (r.name() == QLatin1String("Tag"))
                    rule->tags.append(readTag(r, lossless));
                else {
                    *lossless = false;   // ObjectSizeGreaterThan, …
                    r.skipCurrentElement();
                }
            }
        } else {
            *lossless = false;
            r.skipCurrentElement();
        }
    }
}

LifecycleRule readRule(QXmlStreamReader& r, bool* lossless)
{
    LifecycleRule rule;
    bool hasStatus = false;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("ID")) {
            rule.id = r.readElementText();
        } else if (n == QLatin1String("Status")) {
            const QString s = r.readElementText().trimmed();
            hasStatus = true;
            rule.enabled = s == QLatin1String("Enabled");
            if (!rule.enabled && s != QLatin1String("Disabled"))
                *lossless = false;
        } else if (n == QLatin1String("Filter")) {
            readFilter(r, &rule, lossless);
        } else if (n == QLatin1String("Prefix")) {
            rule.prefix = r.readElementText();
            rule.legacyPrefix = true;
        } else if (n == QLatin1String("Expiration")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("Days"))
                    rule.expirationDays = readInt(r, lossless);
                else if (r.name() == QLatin1String("Date"))
                    rule.expirationDate = readDate(r, lossless);
                else if (r.name() == QLatin1String("ExpiredObjectDeleteMarker"))
                    rule.expiredObjectDeleteMarker = readBool(r, lossless);
                else {
                    *lossless = false;
                    r.skipCurrentElement();
                }
            }
        } else if (n == QLatin1String("Transition")) {
            LifecycleTransition t;
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("Days"))
                    t.days = readInt(r, lossless);
                else if (r.name() == QLatin1String("Date"))
                    t.date = readDate(r, lossless);
                else if (r.name() == QLatin1String("StorageClass"))
                    t.storageClass = r.readElementText().trimmed();
                else {
                    *lossless = false;
                    r.skipCurrentElement();
                }
            }
            rule.transitions.append(t);
        } else if (n == QLatin1String("NoncurrentVersionExpiration")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("NoncurrentDays"))
                    rule.noncurrentDays = readInt(r, lossless);
                else if (r.name() == QLatin1String("NewerNoncurrentVersions"))
                    rule.newerNoncurrentVersions = readInt(r, lossless);
                else {
                    *lossless = false;
                    r.skipCurrentElement();
                }
            }
        } else if (n == QLatin1String("AbortIncompleteMultipartUpload")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("DaysAfterInitiation"))
                    rule.abortIncompleteDays = readInt(r, lossless);
                else {
                    *lossless = false;
                    r.skipCurrentElement();
                }
            }
        } else {
            *lossless = false;   // NoncurrentVersionTransition, …
            r.skipCurrentElement();
        }
    }
    if (!hasStatus)
        *lossless = false;
    return rule;
}

} // namespace

Result<LifecycleConfiguration> parseLifecycleConfiguration(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("LifecycleConfiguration")))
        return *err;
    LifecycleConfiguration out;
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("Rule")) {
            out.rules.append(readRule(r, &out.lossless));
        } else {
            out.lossless = false;
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    return out;
}

Result<QByteArray> buildLifecycleConfiguration(const LifecycleConfiguration& config)
{
    QByteArray out = QByteArray(kXmlHeader) + "<LifecycleConfiguration" + kNs + ">";
    for (const LifecycleRule& rule : config.rules) {
        bool ok = representable(rule.id) && representable(rule.prefix);
        for (const Tag& t : rule.tags)
            ok = ok && representable(t.key) && representable(t.value);
        for (const LifecycleTransition& t : rule.transitions)
            ok = ok && representable(t.storageClass);
        if (!ok)
            return invalid(QStringLiteral("a lifecycle rule contains characters that cannot be sent"));
        out += "<Rule>";
        if (!rule.id.isEmpty())
            out += element("ID", rule.id);
        if (rule.legacyPrefix && rule.tags.isEmpty()) {
            out += element("Prefix", rule.prefix);
        } else {
            out += "<Filter>";
            const auto tag = [](const Tag& t) {
                return "<Tag>" + element("Key", t.key) + element("Value", t.value) + "</Tag>";
            };
            if (rule.tags.isEmpty()) {
                out += element("Prefix", rule.prefix);
            } else if (rule.tags.size() == 1 && rule.prefix.isEmpty()) {
                out += tag(rule.tags.first());
            } else {
                out += "<And>";
                if (!rule.prefix.isEmpty())
                    out += element("Prefix", rule.prefix);
                for (const Tag& t : rule.tags)
                    out += tag(t);
                out += "</And>";
            }
            out += "</Filter>";
        }
        out += element("Status", rule.enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled"));
        for (const LifecycleTransition& t : rule.transitions) {
            out += "<Transition>";
            if (t.days)
                out += element("Days", *t.days);
            else if (t.date.isValid())
                out += element("Date", dateText(t.date));
            out += element("StorageClass", t.storageClass) + "</Transition>";
        }
        if (rule.expirationDays || rule.expirationDate.isValid() || rule.expiredObjectDeleteMarker) {
            out += "<Expiration>";
            if (rule.expirationDays)
                out += element("Days", *rule.expirationDays);
            else if (rule.expirationDate.isValid())
                out += element("Date", dateText(rule.expirationDate));
            else
                out += element("ExpiredObjectDeleteMarker", QStringLiteral("true"));
            out += "</Expiration>";
        }
        if (rule.noncurrentDays) {
            out += "<NoncurrentVersionExpiration>" + element("NoncurrentDays", *rule.noncurrentDays);
            if (rule.newerNoncurrentVersions)
                out += element("NewerNoncurrentVersions", *rule.newerNoncurrentVersions);
            out += "</NoncurrentVersionExpiration>";
        }
        if (rule.abortIncompleteDays) {
            out += "<AbortIncompleteMultipartUpload>" + element("DaysAfterInitiation", *rule.abortIncompleteDays)
                 + "</AbortIncompleteMultipartUpload>";
        }
        out += "</Rule>";
    }
    out += "</LifecycleConfiguration>";
    return out;
}

QString lifecycleProblem(const LifecycleConfiguration& config)
{
    if (config.rules.isEmpty())
        return QStringLiteral("a lifecycle configuration needs at least one rule");
    if (config.rules.size() > 1000)
        return QStringLiteral("a lifecycle configuration has at most 1000 rules");
    QStringList ids;
    for (const LifecycleRule& rule : config.rules) {
        const QString name = rule.id.isEmpty() ? QStringLiteral("a rule") : QStringLiteral("rule \"%1\"").arg(rule.id);
        if (rule.id.size() > 255)
            return QStringLiteral("a rule ID has at most 255 characters");
        if (!rule.id.isEmpty()) {
            if (ids.contains(rule.id))
                return QStringLiteral("rule ID \"%1\" appears twice").arg(rule.id);
            ids.append(rule.id);
        }
        if (rule.legacyPrefix && !rule.tags.isEmpty())
            return QStringLiteral("%1 filters by tags, which needs the <Filter> form").arg(name);
        const bool expiration = rule.expirationDays || rule.expirationDate.isValid() || rule.expiredObjectDeleteMarker;
        if (!expiration && rule.transitions.isEmpty() && !rule.noncurrentDays && !rule.abortIncompleteDays)
            return QStringLiteral("%1 has no action").arg(name);
        if (int(bool(rule.expirationDays)) + int(rule.expirationDate.isValid()) + int(rule.expiredObjectDeleteMarker) > 1)
            return QStringLiteral("%1: expire after days, on a date, or remove expired delete markers — one of them")
                .arg(name);
        if (rule.expiredObjectDeleteMarker && !rule.tags.isEmpty())
            return QStringLiteral("%1: removing expired delete markers can't be combined with a tag filter").arg(name);
        if (rule.expirationDays && *rule.expirationDays < 1)
            return QStringLiteral("%1: expiration days are at least 1").arg(name);
        if (rule.noncurrentDays && *rule.noncurrentDays < 1)
            return QStringLiteral("%1: days for earlier versions are at least 1").arg(name);
        if (rule.newerNoncurrentVersions && !rule.noncurrentDays)
            return QStringLiteral("%1: versions to keep need days for earlier versions").arg(name);
        if (rule.newerNoncurrentVersions && (*rule.newerNoncurrentVersions < 1 || *rule.newerNoncurrentVersions > 100))
            return QStringLiteral("%1: keep 1 to 100 earlier versions").arg(name);
        if (rule.abortIncompleteDays && *rule.abortIncompleteDays < 1)
            return QStringLiteral("%1: days for unfinished uploads are at least 1").arg(name);
        if (rule.abortIncompleteDays && !rule.tags.isEmpty())
            return QStringLiteral("%1: aborting unfinished uploads can't be combined with a tag filter").arg(name);
        for (const LifecycleTransition& t : rule.transitions) {
            if (t.storageClass.isEmpty())
                return QStringLiteral("%1: a transition needs a storage class").arg(name);
            if (bool(t.days) == t.date.isValid())
                return QStringLiteral("%1: a transition happens after days or on a date").arg(name);
            if (t.days && *t.days < 0)
                return QStringLiteral("%1: transition days are not negative").arg(name);
        }
        for (const Tag& tag : rule.tags) {
            if (tag.key.isEmpty())
                return QStringLiteral("%1: a tag needs a key").arg(name);
        }
    }
    return {};
}

// --- Object lock ------------------------------------------------------------------------

Result<ObjectLockConfiguration> parseObjectLockConfiguration(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("ObjectLockConfiguration")))
        return *err;
    ObjectLockConfiguration out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("ObjectLockEnabled")) {
            const QString text = r.readElementText().trimmed();
            out.enabled = text == QLatin1String("Enabled");
            if (!out.enabled && !text.isEmpty())
                out.lossless = false;
        } else if (n == QLatin1String("Rule")) {
            while (r.readNextStartElement()) {
                if (r.name() != QLatin1String("DefaultRetention")) {
                    out.lossless = false;
                    r.skipCurrentElement();
                    continue;
                }
                while (r.readNextStartElement()) {
                    if (r.name() == QLatin1String("Mode")) {
                        out.mode = objectLockModeFrom(r.readElementText().trimmed());
                        if (!out.mode)
                            out.lossless = false;
                    } else if (r.name() == QLatin1String("Days")) {
                        out.days = readInt(r, &out.lossless);
                    } else if (r.name() == QLatin1String("Years")) {
                        out.years = readInt(r, &out.lossless);
                    } else {
                        out.lossless = false;
                        r.skipCurrentElement();
                    }
                }
            }
        } else {
            out.lossless = false;
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    return out;
}

QByteArray buildObjectLockConfiguration(const ObjectLockConfiguration& config)
{
    QByteArray out = QByteArray(kXmlHeader) + "<ObjectLockConfiguration" + kNs + ">";
    if (config.enabled)
        out += "<ObjectLockEnabled>Enabled</ObjectLockEnabled>";
    if (config.mode) {
        out += "<Rule><DefaultRetention>" + element("Mode", objectLockModeName(*config.mode));
        if (config.days)
            out += element("Days", *config.days);
        else if (config.years)
            out += element("Years", *config.years);
        out += "</DefaultRetention></Rule>";
    }
    out += "</ObjectLockConfiguration>";
    return out;
}

QString objectLockProblem(const ObjectLockConfiguration& config)
{
    if (!config.enabled)
        return QStringLiteral("object lock can only be enabled when the bucket is created");
    if (!config.mode)
        return config.days || config.years ? QStringLiteral("a default retention needs a mode") : QString();
    if (bool(config.days) == bool(config.years))
        return QStringLiteral("a default retention lasts a number of days or of years");
    if (config.days && (*config.days < 1 || *config.days > 36500))
        return QStringLiteral("a default retention lasts 1 to 36500 days");
    if (config.years && (*config.years < 1 || *config.years > 100))
        return QStringLiteral("a default retention lasts 1 to 100 years");
    return {};
}

// --- Access ----------------------------------------------------------------------------------

Result<PublicAccessBlockConfiguration> parsePublicAccessBlock(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("PublicAccessBlockConfiguration")))
        return *err;
    PublicAccessBlockConfiguration out;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("BlockPublicAcls"))
            out.blockPublicAcls = readBool(r, &out.lossless);
        else if (n == QLatin1String("IgnorePublicAcls"))
            out.ignorePublicAcls = readBool(r, &out.lossless);
        else if (n == QLatin1String("BlockPublicPolicy"))
            out.blockPublicPolicy = readBool(r, &out.lossless);
        else if (n == QLatin1String("RestrictPublicBuckets"))
            out.restrictPublicBuckets = readBool(r, &out.lossless);
        else {
            out.lossless = false;
            r.skipCurrentElement();
        }
    }
    if (auto err = finish(r))
        return *err;
    return out;
}

QByteArray buildPublicAccessBlock(const PublicAccessBlockConfiguration& config)
{
    const auto flag = [](const char* name, bool on) {
        return element(name, on ? QStringLiteral("true") : QStringLiteral("false"));
    };
    return QByteArray(kXmlHeader) + "<PublicAccessBlockConfiguration" + kNs + ">"
         + flag("BlockPublicAcls", config.blockPublicAcls) + flag("IgnorePublicAcls", config.ignorePublicAcls)
         + flag("BlockPublicPolicy", config.blockPublicPolicy)
         + flag("RestrictPublicBuckets", config.restrictPublicBuckets) + "</PublicAccessBlockConfiguration>";
}

Result<bool> parsePolicyStatus(const QByteArray& body)
{
    QXmlStreamReader r(body);
    if (auto err = openRoot(r, QLatin1StringView("PolicyStatus")))
        return *err;
    bool isPublic = false;
    bool lossless = true;
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("IsPublic"))
            isPublic = readBool(r, &lossless);
        else
            r.skipCurrentElement();
    }
    if (auto err = finish(r))
        return *err;
    return isPublic;
}

Result<AccessControlPolicy> parseAccessControlPolicyDocument(const QByteArray& body)
{
    return xml::parseAccessControlPolicy(body);
}

// --- Policy ------------------------------------------------------------------------------------

QStringList policyProblems(const QByteArray& json)
{
    QStringList out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError) {
        out.append(QStringLiteral("not JSON: %1 (at character %2)").arg(err.errorString()).arg(err.offset));
        return out;
    }
    if (!doc.isObject()) {
        out.append(QStringLiteral("a policy is a JSON object"));
        return out;
    }
    const QJsonObject policy = doc.object();
    const QString version = policy.value(QLatin1String("Version")).toString();
    if (version.isEmpty())
        out.append(QStringLiteral("\"Version\" is missing (use \"2012-10-17\")"));
    else if (version != QLatin1String("2012-10-17") && version != QLatin1String("2008-10-17"))
        out.append(QStringLiteral("unknown \"Version\": %1").arg(version));
    const QJsonValue s = policy.value(QLatin1String("Statement"));
    if (!s.isArray() && !s.isObject()) {
        out.append(QStringLiteral("\"Statement\" is missing"));
        return out;
    }
    const QList<QJsonObject> list = statements(policy);
    if (list.isEmpty())
        out.append(QStringLiteral("\"Statement\" has no entries"));
    for (qsizetype i = 0; i < list.size(); ++i) {
        const QJsonObject& st = list[i];
        const QString where = st.contains(QLatin1String("Sid"))
            ? QStringLiteral("statement \"%1\"").arg(st.value(QLatin1String("Sid")).toString())
            : QStringLiteral("statement %1").arg(i + 1);
        const QString effect = st.value(QLatin1String("Effect")).toString();
        if (effect != QLatin1String("Allow") && effect != QLatin1String("Deny"))
            out.append(QStringLiteral("%1: \"Effect\" is \"Allow\" or \"Deny\"").arg(where));
        if (!st.contains(QLatin1String("Action")) && !st.contains(QLatin1String("NotAction")))
            out.append(QStringLiteral("%1: \"Action\" is missing").arg(where));
        if (!st.contains(QLatin1String("Resource")) && !st.contains(QLatin1String("NotResource")))
            out.append(QStringLiteral("%1: \"Resource\" is missing").arg(where));
        if (!st.contains(QLatin1String("Principal")) && !st.contains(QLatin1String("NotPrincipal")))
            out.append(QStringLiteral("%1: \"Principal\" is missing (a bucket policy names who it applies to)").arg(where));
    }
    return out;
}

PolicyFindings analyzePolicy(const QByteArray& json)
{
    PolicyFindings out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return out;
    for (const QJsonObject& st : statements(doc.object())) {
        const QString effect = st.value(QLatin1String("Effect")).toString();
        if (effect == QLatin1String("Deny")) {
            out.deny = true;
            continue;
        }
        if (effect != QLatin1String("Allow") || !publicPrincipal(st.value(QLatin1String("Principal"))))
            continue;
        out.publicRead = true;
        for (const QString& action : jsonStrings(st.value(QLatin1String("Action")))) {
            const QString a = action.toLower();
            if (a == QLatin1String("*") || a == QLatin1String("s3:*") || a.startsWith(QLatin1String("s3:put"))
                || a.startsWith(QLatin1String("s3:delete")) || a.startsWith(QLatin1String("s3:abort"))
                || a.startsWith(QLatin1String("s3:restore")))
                out.publicWrite = true;
        }
        if (st.contains(QLatin1String("NotAction")))
            out.publicWrite = true;
    }
    return out;
}

// --- Bucket names --------------------------------------------------------------------------------

BucketNameProblem bucketNameProblem(const QString& name)
{
    if (name.size() < 3 || name.size() > 63)
        return BucketNameProblem::Length;
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '.' || u == '-'))
            return BucketNameProblem::Characters;
    }
    const auto alnum = [](QChar c) { return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('z')); };
    if (!alnum(name.front()) || !alnum(name.back()))
        return BucketNameProblem::StartOrEnd;
    if (name.contains(QLatin1String("..")))
        return BucketNameProblem::AdjacentDots;
    if (name.contains(QLatin1String(".-")) || name.contains(QLatin1String("-.")))
        return BucketNameProblem::DotNextToHyphen;
    static const QRegularExpression ip(QStringLiteral("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$"));
    if (ip.match(name).hasMatch())
        return BucketNameProblem::IpAddress;
    if (startsWithAny(name, {"xn--", "sthree-", "amzn-s3-demo-"}))
        return BucketNameProblem::ReservedPrefix;
    if (endsWithAny(name, {"-s3alias", "--ol-s3", ".mrap", "--x-s3", "--table-s3"}))
        return BucketNameProblem::ReservedSuffix;
    return BucketNameProblem::None;
}

QString bucketNameProblemText(BucketNameProblem problem)
{
    switch (problem) {
    case BucketNameProblem::None:            return {};
    case BucketNameProblem::Length:          return QStringLiteral("a bucket name has 3 to 63 characters");
    case BucketNameProblem::Characters:
        return QStringLiteral("a bucket name has lower-case letters, digits, dots and hyphens only");
    case BucketNameProblem::StartOrEnd:
        return QStringLiteral("a bucket name starts and ends with a letter or a digit");
    case BucketNameProblem::AdjacentDots:    return QStringLiteral("a bucket name has no two dots in a row");
    case BucketNameProblem::DotNextToHyphen: return QStringLiteral("a bucket name has no dot next to a hyphen");
    case BucketNameProblem::IpAddress:       return QStringLiteral("a bucket name is no IP address");
    case BucketNameProblem::ReservedPrefix:
        return QStringLiteral("a bucket name does not start with xn--, sthree- or amzn-s3-demo-");
    case BucketNameProblem::ReservedSuffix:
        return QStringLiteral("a bucket name does not end with -s3alias, --ol-s3, .mrap, --x-s3 or --table-s3");
    }
    return {};
}

} // namespace qbucket::s3
