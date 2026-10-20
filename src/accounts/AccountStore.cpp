#include "accounts/AccountStore.h"

#include "diagnostics/Logging.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace qbucket {
namespace {

constexpr auto kGroupPrefix = "account-";

QString defaultPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/accounts.ini");
}

QString groupFor(const QUuid& id)
{
    return QLatin1String(kGroupPrefix) + id.toString(QUuid::WithoutBraces);
}

QString addressingName(s3::AddressingStyle s)
{
    switch (s) {
    case s3::AddressingStyle::Path:          return QStringLiteral("path");
    case s3::AddressingStyle::VirtualHosted: return QStringLiteral("virtual");
    case s3::AddressingStyle::Auto:          break;
    }
    return QStringLiteral("auto");
}

s3::AddressingStyle addressingFrom(const QString& s)
{
    if (s == QLatin1String("path"))
        return s3::AddressingStyle::Path;
    if (s == QLatin1String("virtual"))
        return s3::AddressingStyle::VirtualHosted;
    return s3::AddressingStyle::Auto;
}

QString environmentName(Environment e)
{
    switch (e) {
    case Environment::Staging:     return QStringLiteral("staging");
    case Environment::Development: return QStringLiteral("development");
    case Environment::Production:  break;
    }
    return QStringLiteral("production");
}

// Anything unrecognised is Production — the careful default.
Environment environmentFrom(const QString& s)
{
    if (s == QLatin1String("staging"))
        return Environment::Staging;
    if (s == QLatin1String("development"))
        return Environment::Development;
    return Environment::Production;
}

} // namespace

AccountStore::AccountStore(QObject* parent)
    : AccountStore(defaultPath(), parent)
{
}

AccountStore::AccountStore(const QString& filePath, QObject* parent)
    : QObject(parent),
      m_store(new QSettings(filePath, QSettings::IniFormat, this))
{
    load_();
}

AccountStore::~AccountStore() = default;

QString AccountStore::fileName() const
{
    return m_store->fileName();
}

std::optional<Account> AccountStore::account(const QUuid& id) const
{
    for (const Account& a : m_accounts) {
        if (a.id == id)
            return a;
    }
    return std::nullopt;
}

void AccountStore::load_()
{
    m_accounts.clear();
    for (const QString& group : m_store->childGroups()) {
        if (!group.startsWith(QLatin1String(kGroupPrefix)))
            continue;
        m_store->beginGroup(group);
        Account a;
        a.id = QUuid::fromString(group.mid(int(qstrlen(kGroupPrefix))));
        a.name = m_store->value(QStringLiteral("name")).toString();
        a.providerId = m_store->value(QStringLiteral("provider")).toString();
        a.region = m_store->value(QStringLiteral("region")).toString();
        a.endpoint = m_store->value(QStringLiteral("endpoint")).toString();
        a.addressing = addressingFrom(m_store->value(QStringLiteral("addressing")).toString());
        a.accessKeyId = m_store->value(QStringLiteral("access_key_id")).toString();
        a.environment = environmentFrom(m_store->value(QStringLiteral("environment")).toString());
        a.readOnly = m_store->value(QStringLiteral("read_only"), false).toBool();
        a.externalBuckets = m_store->value(QStringLiteral("external_buckets")).toStringList();
        m_store->endGroup();
        if (a.isValid())
            m_accounts.append(a);
        else
            qCWarning(lcApp).noquote() << "ignoring incomplete account entry" << group;
    }
    sort_();
}

void AccountStore::sort_()
{
    // Case-insensitive first: the same order under every locale (the C
    // locale would put "Zeta" before "alpha").
    std::sort(m_accounts.begin(), m_accounts.end(), [](const Account& a, const Account& b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.name < b.name;
    });
}

bool AccountStore::save(const Account& account)
{
    if (!account.isValid())
        return false;
    m_store->beginGroup(groupFor(account.id));
    m_store->remove(QString());   // drop stale keys of this account
    m_store->setValue(QStringLiteral("schema"), 1);
    m_store->setValue(QStringLiteral("name"), account.name.trimmed());
    m_store->setValue(QStringLiteral("provider"), account.providerId);
    m_store->setValue(QStringLiteral("region"), account.region);
    m_store->setValue(QStringLiteral("endpoint"), account.endpoint);
    m_store->setValue(QStringLiteral("addressing"), addressingName(account.addressing));
    m_store->setValue(QStringLiteral("access_key_id"), account.accessKeyId);
    m_store->setValue(QStringLiteral("environment"), environmentName(account.environment));
    m_store->setValue(QStringLiteral("read_only"), account.readOnly);
    m_store->setValue(QStringLiteral("external_buckets"), account.externalBuckets);
    m_store->endGroup();
    m_store->sync();

    const auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                                 [&](const Account& a) { return a.id == account.id; });
    Account stored = account;
    stored.name = account.name.trimmed();
    if (it != m_accounts.end())
        *it = stored;
    else
        m_accounts.append(stored);
    sort_();
    emit accountsChanged();
    return true;
}

bool AccountStore::remove(const QUuid& id)
{
    const auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                                 [&](const Account& a) { return a.id == id; });
    if (it == m_accounts.end())
        return false;
    m_accounts.erase(it);
    m_store->remove(groupFor(id));
    m_store->sync();
    emit accountsChanged();
    return true;
}

} // namespace qbucket
