#include "controllers/AccountImporter.h"

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"

#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QUrl>

namespace qbucket {
namespace {

const QString kDefaultRegion = QStringLiteral("us-east-1");

// scheme://host[:port] in lower case, for comparing endpoints.
QString comparable(const QUrl& url)
{
    if (!url.isValid() || url.host().isEmpty())
        return {};
    QString out = url.scheme().toLower() + QStringLiteral("://") + url.host().toLower();
    if (url.port() > 0 && !(url.scheme() == QLatin1String("https") && url.port() == 443)
        && !(url.scheme() == QLatin1String("http") && url.port() == 80))
        out += QLatin1Char(':') + QString::number(url.port());
    return out;
}

QString effectiveEndpoint(const QString& providerId, const QString& region, const QString& endpoint)
{
    if (!endpoint.isEmpty())
        return comparable(QUrl(endpoint));
    return comparable(s3::ProviderRegistry::builtin().findOrGeneric(providerId)->endpointFor(region));
}

QString displayName(const QString& providerId)
{
    return s3::ProviderRegistry::builtin().findOrGeneric(providerId)->displayName();
}

// The provider by the endpoint: a preset → that profile and region, no
// endpoint stored (it follows the profile); otherwise empty.
bool suggestByEndpoint(ImportCandidate* c, const QString& endpoint)
{
    const std::optional<s3::ProviderRegistry::EndpointMatch> m =
        s3::ProviderRegistry::builtin().matchEndpoint(QUrl(endpoint));
    if (!m)
        return false;
    c->providerId = m->profileId;
    c->region = m->region;
    c->endpoint.clear();
    c->why = AccountImporter::tr("The endpoint is the %1 preset for %2.").arg(displayName(m->profileId), m->region);
    return true;
}

void finish(QList<ImportCandidate>* list, const QList<Account>& existing)
{
    QSet<QString> taken;
    for (const Account& a : existing)
        taken.insert(a.name.toLower());
    for (ImportCandidate& c : *list) {
        QString name = c.name.trimmed().isEmpty() ? QStringLiteral("imported") : c.name.trimmed();
        for (int n = 2; taken.contains(name.toLower()); ++n)
            name = QStringLiteral("%1 (%2)").arg(c.name.trimmed()).arg(n);
        c.name = name;
        taken.insert(name.toLower());
        if (!c.importable() || c.accessKeyId.isEmpty())
            continue;
        const QString endpoint = effectiveEndpoint(c.providerId, c.region, c.endpoint);
        for (const Account& a : existing) {
            if (a.accessKeyId == c.accessKeyId && effectiveEndpoint(a.providerId, a.region, a.endpoint) == endpoint) {
                c.existing = a.id;
                break;
            }
        }
    }
}

s3::AddressingStyle addressingFrom(const QString& style)
{
    if (style == QLatin1String("path"))
        return s3::AddressingStyle::Path;
    if (style == QLatin1String("virtual"))
        return s3::AddressingStyle::VirtualHosted;
    return s3::AddressingStyle::Auto;
}

} // namespace

AccountImporter::AccountImporter(AccountStore* accounts, SecretStore* secrets, QObject* parent)
    : QObject(parent),
      m_accounts(accounts),
      m_secrets(secrets)
{
}

QStringList AccountImporter::defaultFiles(Source source)
{
    QStringList out;
    const auto readable = [](const QString& path) { return !path.isEmpty() && QFileInfo(path).isReadable(); };
    if (source == Source::Aws) {
        for (const QString& p : {s3::AwsConfigFiles::credentialsPath(), s3::AwsConfigFiles::configPath()}) {
            if (readable(p))
                out.append(p);
        }
        return out;
    }
    for (const QString& p : RcloneConfigReader::candidatePaths()) {
        if (readable(p))
            return {p};
    }
    return out;
}

QList<ImportCandidate> AccountImporter::fromAws(const QList<s3::AwsProfileEntry>& profiles, const QList<Account>& existing)
{
    QList<ImportCandidate> out;
    for (const s3::AwsProfileEntry& e : profiles) {
        ImportCandidate c;
        c.origin = tr("AWS profile “%1”").arg(e.name);
        c.name = e.name;
        c.accessKeyId = e.accessKeyId;
        c.secretAccessKey = e.secretAccessKey;
        c.region = e.region.isEmpty() ? kDefaultRegion : e.region;
        c.addressing = addressingFrom(e.addressingStyle);
        if (e.endpointUrl.isEmpty()) {
            c.providerId = QStringLiteral("aws");
            c.why = tr("No endpoint is set: Amazon S3.");
        } else if (!suggestByEndpoint(&c, e.endpointUrl)) {
            c.providerId = QStringLiteral("generic");
            c.endpoint = e.endpointUrl;
            c.why = tr("No provider preset has this endpoint: generic S3.");
        }
        if (!e.indirectCredentials.isEmpty())
            c.skipReason = tr("Gets its credentials elsewhere (%1) — not supported.").arg(e.indirectCredentials.join(QStringLiteral(", ")));
        else if (!e.sessionToken.isEmpty())
            c.skipReason = tr("Temporary credentials (a session token) — not supported.");
        else if (e.accessKeyId.isEmpty() || e.secretAccessKey.isEmpty())
            c.skipReason = tr("No access key and secret in the files.");
        out.append(c);
    }
    finish(&out, existing);
    return out;
}

QList<ImportCandidate> AccountImporter::fromRclone(const RcloneConfigReader::Result& config, const QList<Account>& existing)
{
    QList<ImportCandidate> out;
    for (const RcloneRemote& r : config.remotes) {
        ImportCandidate c;
        c.origin = tr("rclone remote “%1”").arg(r.name);
        c.name = r.name;
        c.accessKeyId = r.accessKeyId;
        c.secretAccessKey = r.secretAccessKey;
        c.region = r.region.isEmpty() ? kDefaultRegion : r.region;
        if (r.forcePathStyle)
            c.addressing = *r.forcePathStyle ? s3::AddressingStyle::Path : s3::AddressingStyle::VirtualHosted;
        QString endpoint = r.endpoint;
        if (!endpoint.isEmpty() && !endpoint.contains(QLatin1String("://")))
            endpoint.prepend(QStringLiteral("https://"));   // rclone's default
        const QString provider = r.provider.toLower();
        if (!endpoint.isEmpty() && suggestByEndpoint(&c, endpoint)) {
            // c filled in
        } else if (provider == QLatin1String("aws") && endpoint.isEmpty()) {
            c.providerId = QStringLiteral("aws");
            c.why = tr("rclone says provider AWS.");
        } else if (provider == QLatin1String("ceph") && !endpoint.isEmpty()) {
            c.providerId = QStringLiteral("ceph");
            c.endpoint = endpoint;
            c.why = tr("rclone says provider Ceph.");
        } else {
            c.providerId = QStringLiteral("generic");
            c.endpoint = endpoint;
            c.why = tr("No provider preset has this endpoint: generic S3.");
        }
        if (r.envAuth && r.accessKeyId.isEmpty())
            c.skipReason = tr("Takes its credentials from the environment (env_auth) — not supported.");
        else if (r.accessKeyId.isEmpty() || r.secretAccessKey.isEmpty())
            c.skipReason = tr("No access key and secret in the configuration.");
        else if (c.providerId == QLatin1String("generic") && c.endpoint.isEmpty())
            c.skipReason = tr("No endpoint in the configuration.");
        out.append(c);
    }
    finish(&out, existing);
    return out;
}

Account AccountImporter::accountFor(const ImportCandidate& c, Environment environment, bool readOnly)
{
    Account a;
    a.id = QUuid::createUuid();
    a.name = c.name.trimmed();
    a.providerId = c.providerId;
    a.region = c.region;
    a.endpoint = c.endpoint;
    a.addressing = c.addressing;
    a.accessKeyId = c.accessKeyId;
    a.environment = environment;
    a.readOnly = readOnly;
    return a;
}

int AccountImporter::read(Source source, const QStringList& files)
{
    const int request = ++m_requests;
    auto* relay = new QObject;
    relay->moveToThread(thread());
    const QPointer<AccountImporter> self(this);
    QThreadPool::globalInstance()->start([relay, self, source, files, request] {
        QString problem;
        QByteArray credentials;
        QByteArray config;
        for (const QString& path : files) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                problem = tr("%1 could not be read: %2").arg(path, f.errorString());
                break;
            }
            const QByteArray content = f.readAll();
            if (source == Source::Aws && QFileInfo(path).fileName().contains(QLatin1String("config")))
                config += content + '\n';
            else
                credentials += content + '\n';
        }
        if (files.isEmpty())
            problem = tr("No configuration file was found. Choose one.");
        QList<s3::AwsProfileEntry> aws;
        RcloneConfigReader::Result rclone;
        if (problem.isEmpty()) {
            if (source == Source::Aws)
                aws = s3::AwsConfigFiles::parse(credentials, config);
            else
                rclone = RcloneConfigReader::parse(credentials);
            if (rclone.encrypted)
                problem = tr("The rclone configuration is encrypted. Decrypt it first "
                             "(rclone config encryption remove), or set the accounts up by hand.");
        }
        QMetaObject::invokeMethod(relay, [relay, self, source, aws, rclone, problem, request] {
            if (self) {
                const QList<Account> existing = self->m_accounts->accounts();
                QList<ImportCandidate> out;
                if (problem.isEmpty())
                    out = source == Source::Aws ? fromAws(aws, existing) : fromRclone(rclone, existing);
                const QString none = problem.isEmpty() && out.isEmpty()
                    ? (source == Source::Aws ? tr("The files hold no profiles.") : tr("The file holds no S3 remotes."))
                    : problem;
                emit self->candidatesReady(out, none, request);
            }
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
    return request;
}

void AccountImporter::save(const QList<ImportCandidate>& chosen, Environment environment, bool readOnly)
{
    if (m_saving)
        return;
    m_saving = true;
    m_queue = chosen;
    m_environment = environment;
    m_readOnly = readOnly;
    m_saved = 0;
    m_sessionOnly = false;
    m_errors.clear();
    saveNext_();
}

void AccountImporter::saveNext_()
{
    while (!m_queue.isEmpty() && !m_queue.first().importable())
        m_queue.removeFirst();
    if (m_queue.isEmpty()) {
        m_saving = false;
        emit saveFinished(m_saved, m_sessionOnly, m_errors);
        return;
    }
    const ImportCandidate c = m_queue.takeFirst();
    const Account a = accountFor(c, m_environment, m_readOnly);
    // The secret first: an account never exists without it.
    m_secrets->write(a.id, SecretStore::Kind::SecretAccessKey, c.secretAccessKey, this,
                     [this, a](const SecretStore::Result& r) {
        if (!r.ok())
            m_errors.append(tr("“%1”: the keychain refused the secret: %2").arg(a.name, r.message));
        else if (!m_accounts->save(a)) {
            m_errors.append(tr("“%1”: the account could not be saved.").arg(a.name));
            m_secrets->removeAll(a.id, this, [](const SecretStore::Result&) {});   // no orphaned secret
        } else {
            ++m_saved;
            m_sessionOnly = m_sessionOnly || r.sessionOnly;
        }
        saveNext_();
    });
}

} // namespace qbucket
