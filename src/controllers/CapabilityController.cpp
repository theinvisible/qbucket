#include "controllers/CapabilityController.h"

#include "accounts/AccountStore.h"
#include "accounts/CapabilityStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/ActivityLog.h"
#include "controllers/ConnectionTester.h"

#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QJsonObject>
#include <QLocale>

namespace qbucket {

namespace {

bool expired(const std::optional<s3::CapabilitySet>& set)
{
    return !set || s3::capabilities::isExpired(*set, QDateTime::currentDateTimeUtc());
}

bool backedOff(const QDateTime& lastFailure)
{
    return lastFailure.isValid()
        && lastFailure.secsTo(QDateTime::currentDateTimeUtc()) < CapabilityController::kRetryAfterFailureSecs;
}

} // namespace

CapabilityController::CapabilityController(AccountStore* accounts, AccountSessions* sessions,
                                           CapabilityStore* store, ActivityLog* log, QObject* parent)
    : QObject(parent),
      m_accounts(accounts),
      m_sessions(sessions),
      m_store(store),
      m_log(log)
{
    connect(m_sessions, &AccountSessions::sessionStarted, this, &CapabilityController::onSessionStarted_);
    connect(m_sessions, &AccountSessions::sessionStopped, this, &CapabilityController::onSessionStopped_);
    connect(m_sessions, &AccountSessions::capabilityLearned, this, &CapabilityController::onLearned_);
    connect(m_accounts, &AccountStore::accountsChanged, this, &CapabilityController::onAccountsChanged_);
    m_timer.setInterval(kRevalidateMs);
    connect(&m_timer, &QTimer::timeout, this, &CapabilityController::revalidate_);
    m_timer.start();
}

CapabilityController::~CapabilityController() = default;

// --- session lifecycle -------------------------------------------------------

void CapabilityController::onSessionStarted_(const QUuid& account)
{
    std::shared_ptr<AccountCaps>& slot = m_caps[account];
    if (!slot)
        slot = std::make_shared<AccountCaps>();
    const std::shared_ptr<AccountCaps> caps = slot;
    caps->run = std::make_unique<QObject>();
    if (caps->loaded) {
        afterLoad_(account);
        return;
    }
    if (caps->loading)
        return;
    caps->loading = true;
    const std::weak_ptr<AccountCaps> weak = caps;
    m_store->load(account, this, [this, account, weak](const CapabilityStore::Snapshot& snap) {
        const std::shared_ptr<AccountCaps> c = weak.lock();
        if (!c)
            return;   // the account went meanwhile
        c->loading = false;
        c->loaded = true;
        c->server = snap.server;
        for (const s3::CapabilitySet& b : snap.buckets)
            c->buckets[b.bucket].set = b;
        for (const QString& problem : snap.problems)
            m_log->add(ActivityLog::Level::Warning, tr("Stored capabilities skipped: %1").arg(problem));
        afterLoad_(account);
    });
}

void CapabilityController::onSessionStopped_(const QUuid& account)
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!caps)
        return;
    caps->run.reset();   // its probes' answers are dropped
    caps->probingServer = false;
    for (auto& [name, b] : caps->buckets) {
        b.probing = false;
        finishWaiters_(b, false);
    }
}

void CapabilityController::afterLoad_(const QUuid& account)
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!caps || !caps->loaded || !m_sessions->hasSession(account))
        return;
    const s3::Result<s3::Session*> session = m_sessions->session(account);
    if (!session)
        return;
    // The library refuses what is known to be unsupported without sending it.
    if (caps->server)
        (*session)->setCapabilities(*caps->server);
    for (const auto& [name, b] : caps->buckets) {
        if (b.set)
            (*session)->setCapabilities(*b.set);
    }
    emit capabilitiesUpdated(account, QString());

    if (expired(caps->server) && !caps->probingServer && !backedOff(caps->serverFailure))
        probeServer_(account);
    QStringList due;
    for (auto& [name, b] : caps->buckets) {
        if (b.probing)
            continue;
        if (!b.waiters.empty()) {
            const int maxAge = std::min_element(b.waiters.begin(), b.waiters.end(), [](const Waiter& x, const Waiter& y) {
                                   return x.maxAgeSecs < y.maxAgeSecs;
                               })->maxAgeSecs;
            if (youngerThan_(b.set, maxAge))
                finishWaiters_(b, true);
            else
                due.append(name);
        } else if (caps->opened.contains(name) && expired(b.set) && !backedOff(b.lastFailure)) {
            due.append(name);
        }
    }
    for (const QString& opened : std::as_const(caps->opened)) {
        if (caps->buckets.count(opened) == 0)
            due.append(opened);
    }
    for (const QString& bucket : std::as_const(due))
        probeBucket_(account, bucket);
}

void CapabilityController::onAccountsChanged_()
{
    // Results of a removed account, or of an account now pointing at
    // another server, are gone.
    const QList<QUuid> known = m_caps.keys();
    for (const QUuid& id : known) {
        if (!m_accounts->account(id)) {
            m_caps.remove(id);
            m_store->removeAll(id);
        }
    }
}

// --- probing -----------------------------------------------------------------

void CapabilityController::bucketOpened(const QUuid& account, const QString& bucket)
{
    if (bucket.isEmpty())
        return;
    std::shared_ptr<AccountCaps>& slot = m_caps[account];
    if (!slot)
        slot = std::make_shared<AccountCaps>();
    slot->opened.insert(bucket);
    if (!slot->loaded) {
        if (!slot->loading && m_sessions->hasSession(account))
            onSessionStarted_(account);
        return;
    }
    BucketCaps& b = slot->buckets[bucket];
    if (!b.probing && expired(b.set) && !backedOff(b.lastFailure))
        probeBucket_(account, bucket);
}

void CapabilityController::recheck(const QUuid& account, const QString& bucket)
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!caps || !caps->loaded)
        return;
    if (bucket.isEmpty()) {
        caps->serverFailure = QDateTime();
        if (!caps->probingServer)
            probeServer_(account);
        return;
    }
    BucketCaps& b = caps->buckets[bucket];
    b.lastFailure = QDateTime();
    if (!b.probing)
        probeBucket_(account, bucket);
}

void CapabilityController::ensureFresh(const QUuid& account, const QString& bucket, int maxAgeSecs,
                                       QObject* context, std::function<void(bool)> done)
{
    std::shared_ptr<AccountCaps>& slot = m_caps[account];
    if (!slot)
        slot = std::make_shared<AccountCaps>();
    BucketCaps& b = slot->buckets[bucket];
    if (slot->loaded && youngerThan_(b.set, maxAgeSecs)) {
        const QPointer<QObject> guard(context);
        QMetaObject::invokeMethod(this, [guard, done = std::move(done)] {
            if (guard)
                done(true);
        }, Qt::QueuedConnection);
        return;
    }
    b.waiters.push_back({maxAgeSecs, std::move(done), QPointer<QObject>(context)});
    if (!m_sessions->hasSession(account)) {
        // Starting the session loads the stored results, then probes.
        if (!m_sessions->session(account))
            finishWaiters_(b, false);
        return;
    }
    if (!slot->loaded) {
        if (!slot->loading)
            onSessionStarted_(account);
        return;
    }
    if (!b.probing)
        probeBucket_(account, bucket);
}

void CapabilityController::probeServer_(const QUuid& account)
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    const s3::Result<s3::Session*> session = m_sessions->session(account);
    if (!caps || !caps->run || !session)
        return;
    caps->probingServer = true;
    (*session)->probeServer(caps->run.get(), [this, account](const s3::Result<s3::CapabilitySet>& r) {
        const std::shared_ptr<AccountCaps> c = m_caps.value(account);
        if (!c)
            return;
        c->probingServer = false;
        if (!r) {
            c->serverFailure = QDateTime::currentDateTimeUtc();
            m_log->add(ActivityLog::Level::Warning,
                       tr("Could not check what the server of “%1” supports: %2")
                           .arg(m_accounts->account(account).value_or(Account{}).name,
                                ConnectionTester::describe(r.error())));
            return;
        }
        c->serverFailure = QDateTime();
        apply_(account, *r);
    });
}

void CapabilityController::probeBucket_(const QUuid& account, const QString& bucket)
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    const s3::Result<s3::Session*> session = m_sessions->session(account);
    if (!caps || !caps->run || !session)
        return;
    caps->buckets[bucket].probing = true;
    (*session)->probeBucket(bucket, caps->run.get(), [this, account, bucket](const s3::Result<s3::CapabilitySet>& r) {
        const std::shared_ptr<AccountCaps> c = m_caps.value(account);
        if (!c)
            return;
        BucketCaps& b = c->buckets[bucket];
        b.probing = false;
        if (!r) {
            b.lastFailure = QDateTime::currentDateTimeUtc();
            m_log->add(ActivityLog::Level::Warning,
                       tr("Could not check what bucket “%1” supports: %2")
                           .arg(bucket, ConnectionTester::describe(r.error())));
            finishWaiters_(b, false);
            return;
        }
        b.lastFailure = QDateTime();
        apply_(account, *r);
    });
}

void CapabilityController::apply_(const QUuid& account, const s3::CapabilitySet& fresh)
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!caps)
        return;
    const bool server = fresh.scope == s3::CapabilityScope::Server;
    std::optional<s3::CapabilitySet>& slot = server ? caps->server : caps->buckets[fresh.bucket].set;
    const s3::CapabilitySet before = slot.value_or(s3::CapabilitySet{});
    const s3::CapabilitySet merged = slot ? s3::capabilities::merge(*slot, fresh) : fresh;
    // The very first answer is no "change".
    const QList<s3::CapabilityChange> changes = slot ? s3::capabilities::diff(before, merged)
                                                     : QList<s3::CapabilityChange>{};
    slot = merged;
    m_store->save(account, merged);
    if (const s3::Result<s3::Session*> session = m_sessions->session(account))
        (*session)->setCapabilities(merged);
    if (!server)
        finishWaiters_(caps->buckets[fresh.bucket], true);

    if (!changes.isEmpty()) {
        const QString name = m_accounts->account(account).value_or(Account{}).name;
        const QString where = server ? name : tr("%1 › %2").arg(name, fresh.bucket);
        QList<QJsonObject> history;
        for (const s3::CapabilityChange& c : changes) {
            history.append(QJsonObject{
                {QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {QStringLiteral("scope"), server ? QStringLiteral("server") : QStringLiteral("bucket")},
                {QStringLiteral("bucket"), fresh.bucket},
                {QStringLiteral("feature"), s3::featureId(c.feature)},
                {QStringLiteral("before"), s3::stateName(c.before)},
                {QStringLiteral("after"), s3::stateName(c.after)},
            });
            const bool worse = c.after != s3::CapabilityState::Supported;
            m_log->add(worse ? ActivityLog::Level::Warning : ActivityLog::Level::Info,
                       tr("%1: %2 changed from “%3” to “%4”.")
                           .arg(where, featureName(c.feature), stateText(c.before), stateText(c.after)));
        }
        m_store->appendHistory(account, history);
        emit changesDetected(account, fresh.bucket, changes);
    }
    emit capabilitiesUpdated(account, fresh.bucket);
}

void CapabilityController::onLearned_(const QUuid& account, const QString& bucket, const QString& id)
{
    const std::optional<s3::Feature> f = s3::featureFromId(id);
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!f || !caps || bucket.isEmpty())
        return;
    std::optional<s3::CapabilitySet>& slot = caps->buckets[bucket].set;
    if (!slot) {
        slot = s3::CapabilitySet{};
        slot->scope = s3::CapabilityScope::Bucket;
        slot->bucket = bucket;
    }
    s3::CapabilitySet learned = *slot;
    learned.set(*f, {s3::CapabilityState::Unsupported, s3::CapabilitySource::Learned,
                     QDateTime::currentDateTimeUtc(), 0, QString(), false, 0});
    // Same path as a probe: saved, reported, kept by the session.
    const s3::CapabilityState before = slot->state(*f);
    slot = learned;
    m_store->save(account, learned);
    if (before != s3::CapabilityState::Unsupported) {
        const QString name = m_accounts->account(account).value_or(Account{}).name;
        m_store->appendHistory(account, {QJsonObject{
            {QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("scope"), QStringLiteral("bucket")},
            {QStringLiteral("bucket"), bucket},
            {QStringLiteral("feature"), id},
            {QStringLiteral("before"), s3::stateName(before)},
            {QStringLiteral("after"), s3::stateName(s3::CapabilityState::Unsupported)},
        }});
        m_log->add(ActivityLog::Level::Warning,
                   tr("%1 › %2: the server refused %3; it is not offered any more.")
                       .arg(name, bucket, featureName(*f)));
        emit changesDetected(account, bucket, {{*f, before, s3::CapabilityState::Unsupported}});
    }
    emit capabilitiesUpdated(account, bucket);
}

void CapabilityController::revalidate_()
{
    const auto ids = m_caps.keys();
    for (const QUuid& id : ids) {
        const std::shared_ptr<AccountCaps> caps = m_caps.value(id);
        if (!caps->loaded || !caps->run || !m_sessions->hasSession(id))
            continue;
        if (expired(caps->server) && !caps->probingServer && !backedOff(caps->serverFailure))
            probeServer_(id);
        for (const QString& bucket : std::as_const(caps->opened)) {
            BucketCaps& b = caps->buckets[bucket];
            if (!b.probing && expired(b.set) && !backedOff(b.lastFailure))
                probeBucket_(id, bucket);
        }
    }
}

void CapabilityController::finishWaiters_(BucketCaps& caps, bool fresh)
{
    std::vector<Waiter> waiters = std::move(caps.waiters);
    caps.waiters.clear();
    for (Waiter& w : waiters) {
        QMetaObject::invokeMethod(this, [w = std::move(w), fresh] {
            if (w.context)
                w.done(fresh);
        }, Qt::QueuedConnection);
    }
}

bool CapabilityController::youngerThan_(const std::optional<s3::CapabilitySet>& set, int secs)
{
    return set && set->probedAt.isValid() && set->probedAt.secsTo(QDateTime::currentDateTimeUtc()) < secs;
}

// --- queries -----------------------------------------------------------------

void CapabilityController::loadHistory(const QUuid& account, QObject* context,
                                       std::function<void(const QList<QJsonObject>&)> done)
{
    m_store->loadHistory(account, context, std::move(done));
}

bool CapabilityController::isLoaded(const QUuid& account) const
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    return caps && caps->loaded;
}

bool CapabilityController::isProbing(const QUuid& account, const QString& bucket) const
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!caps)
        return false;
    if (bucket.isEmpty())
        return caps->probingServer;
    const auto it = caps->buckets.find(bucket);
    return it != caps->buckets.end() && it->second.probing;
}

s3::CapabilitySet CapabilityController::serverSet(const QUuid& account) const
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    return caps ? caps->server.value_or(s3::CapabilitySet{}) : s3::CapabilitySet{};
}

std::optional<s3::CapabilitySet> CapabilityController::bucketSet(const QUuid& account, const QString& bucket) const
{
    const std::shared_ptr<AccountCaps> caps = m_caps.value(account);
    if (!caps)
        return std::nullopt;
    const auto it = caps->buckets.find(bucket);
    return it == caps->buckets.end() ? std::nullopt : it->second.set;
}

QStringList CapabilityController::knownBuckets(const QUuid& account) const
{
    QStringList out;
    if (const std::shared_ptr<AccountCaps> caps = m_caps.value(account)) {
        for (const auto& [name, b] : caps->buckets) {
            if (b.set)
                out.append(name);
        }
    }
    return out;
}

s3::CapabilitySet CapabilityController::profileSet(const QUuid& account) const
{
    const std::optional<Account> a = m_accounts->account(account);
    return s3::ProviderRegistry::builtin().findOrGeneric(a ? a->providerId : QString())->capabilities();
}

s3::CapabilityEntry CapabilityController::entry(const QUuid& account, const QString& bucket, s3::Feature f) const
{
    if (const std::optional<s3::CapabilitySet> b = bucketSet(account, bucket)) {
        const s3::CapabilityEntry e = b->entry(f);
        if (e.state != s3::CapabilityState::Unknown)
            return e;
    }
    s3::CapabilityEntry s = serverSet(account).entry(f);
    if (s.state != s3::CapabilityState::Unknown)
        return s;
    const s3::CapabilityEntry p = profileSet(account).entry(f);
    return p.state != s3::CapabilityState::Unknown ? p : s;
}

s3::Decision CapabilityController::decide(const QUuid& account, const QString& bucket, s3::Feature f,
                                          bool destructive) const
{
    switch (entry(account, bucket, f).state) {
    case s3::CapabilityState::Supported:
        return s3::Decision::Allowed;
    case s3::CapabilityState::Unsupported:
    case s3::CapabilityState::NotPermitted:
        return s3::Decision::Blocked;
    case s3::CapabilityState::Unknown:
        break;
    }
    // Unknown never unlocks a destructive operation.
    return destructive ? s3::Decision::Blocked : s3::Decision::AllowedUnverified;
}

// --- texts -------------------------------------------------------------------

QString CapabilityController::featureName(s3::Feature f)
{
    using F = s3::Feature;
    switch (f) {
    case F::BucketVersioning: return tr("Versioning");
    case F::ObjectLock: return tr("Object lock");
    case F::LegalHold: return tr("Legal hold");
    case F::Retention: return tr("Retention");
    case F::Lifecycle: return tr("Lifecycle rules");
    case F::Cors: return tr("CORS");
    case F::BucketPolicy: return tr("Bucket policy");
    case F::PolicyStatus: return tr("Policy status");
    case F::BucketAcl: return tr("Bucket ACL");
    case F::ObjectAcl: return tr("Object ACL");
    case F::BucketTagging: return tr("Bucket tags");
    case F::ObjectTagging: return tr("Object tags");
    case F::BucketEncryption: return tr("Default encryption");
    case F::SseC: return tr("Customer-provided keys (SSE-C)");
    case F::Website: return tr("Static website");
    case F::Replication: return tr("Replication");
    case F::Logging: return tr("Access logging");
    case F::Notifications: return tr("Event notifications");
    case F::PublicAccessBlock: return tr("Public access block");
    case F::OwnershipControls: return tr("Ownership controls");
    case F::MultipartListing: return tr("Listing multipart uploads");
    case F::ObjectVersionsListing: return tr("Listing object versions");
    case F::ConditionalWrites: return tr("Conditional writes");
    case F::CrossBucketCopy: return tr("Copying between buckets");
    case F::BatchDelete: return tr("Deleting many objects at once");
    case F::Presign: return tr("Presigned links");
    }
    return s3::featureId(f);
}

QString CapabilityController::stateText(s3::CapabilityState state)
{
    switch (state) {
    case s3::CapabilityState::Supported: return tr("Supported");
    case s3::CapabilityState::Unsupported: return tr("Not supported");
    case s3::CapabilityState::NotPermitted: return tr("Not permitted");
    case s3::CapabilityState::Unknown: return tr("Unknown");
    }
    return {};
}

QString CapabilityController::sourceText(s3::CapabilitySource source)
{
    switch (source) {
    case s3::CapabilitySource::Probed: return tr("Checked");
    case s3::CapabilitySource::Learned: return tr("Learned from a request");
    case s3::CapabilitySource::Profile: return tr("Provider profile");
    }
    return {};
}

QString CapabilityController::reason(s3::Feature f, const s3::CapabilityEntry& e, bool destructive)
{
    const QString name = featureName(f);
    QString detail;
    if (e.source == s3::CapabilitySource::Profile) {
        detail = tr("known from the provider profile");
    } else {
        const QString when = e.checkedAt.isValid()
                                 ? QLocale().toString(e.checkedAt.toLocalTime(), QLocale::ShortFormat)
                                 : QString();
        detail = e.httpStatus > 0 ? tr("HTTP %1 %2, %3").arg(e.httpStatus).arg(e.s3Code, when).trimmed()
                                  : when;
    }
    switch (e.state) {
    case s3::CapabilityState::Supported:
        return {};
    case s3::CapabilityState::Unsupported:
        if (e.s3Code == QLatin1String("ProfileOverride:WritesInterceptedByProxy"))
            return tr("%1 is switched off for this provider: its proxy mishandles these requests.").arg(name);
        return tr("The server does not support %1 (%2).").arg(name, detail);
    case s3::CapabilityState::NotPermitted:
        return tr("These credentials may not use %1 (%2).").arg(name, detail);
    case s3::CapabilityState::Unknown:
        return destructive ? tr("%1 is not verified on this server; destructive actions need a confirmed "
                                "capability.").arg(name)
                           : tr("%1 is not verified on this server.").arg(name);
    }
    return {};
}

} // namespace qbucket
