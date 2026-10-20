#pragma once

#include <qbucket/s3/Capabilities.h>

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace qbucket {

class AccountSessions;
class AccountStore;
class ActivityLog;
class CapabilityStore;

// The app side of the capability safeguard (PLAN.md §3.3). Per account:
// loads the persisted results when its session starts and hands them to
// the session (library short-circuit), probes the server on connect and a
// bucket when it is opened, re-probes what has expired (24 h server, 10 min
// bucket, sooner with back-off for Unknown), merges asymmetrically
// (downgrades at once, upgrades only after a definitive answer, a failed
// probe never flips anything) and reports every state change — log,
// signal, history file. Answers decide() for the OperationController.
//
// Thread affinity: GUI thread.
class CapabilityController : public QObject {
    Q_OBJECT

public:
    // How often expired results are looked for.
    static constexpr int kRevalidateMs = 60'000;
    // A whole probe that failed (network, 5xx) is tried again after this.
    static constexpr int kRetryAfterFailureSecs = 5 * 60;

    CapabilityController(AccountStore* accounts, AccountSessions* sessions, CapabilityStore* store,
                         ActivityLog* log, QObject* parent = nullptr);
    ~CapabilityController() override;

    // The browser opened a bucket: probe it unless its results are fresh.
    void bucketOpened(const QUuid& account, const QString& bucket);
    // Probe now, fresh or not (the capabilities page's "Check Again").
    void recheck(const QUuid& account, const QString& bucket = {});
    // Before a destructive operation: results younger than maxAgeSecs are
    // fine, otherwise the bucket is probed first. `done(true)` once the
    // results are fresh, `done(false)` if the probe failed. Queued, never
    // inside the call; dropped with the context.
    void ensureFresh(const QUuid& account, const QString& bucket, int maxAgeSecs, QObject* context,
                     std::function<void(bool fresh)> done);

    [[nodiscard]] bool isLoaded(const QUuid& account) const;
    [[nodiscard]] bool isProbing(const QUuid& account, const QString& bucket = {}) const;
    [[nodiscard]] s3::CapabilitySet serverSet(const QUuid& account) const;
    [[nodiscard]] std::optional<s3::CapabilitySet> bucketSet(const QUuid& account, const QString& bucket) const;
    [[nodiscard]] QStringList knownBuckets(const QUuid& account) const;
    // The profile's static knowledge for the account's provider.
    [[nodiscard]] s3::CapabilitySet profileSet(const QUuid& account) const;

    // The account's change history (oldest first), from the store.
    void loadHistory(const QUuid& account, QObject* context, std::function<void(const QList<QJsonObject>&)> done);

    // Bucket result over server result over profile knowledge.
    [[nodiscard]] s3::CapabilityEntry entry(const QUuid& account, const QString& bucket, s3::Feature f) const;
    [[nodiscard]] s3::Decision decide(const QUuid& account, const QString& bucket, s3::Feature f,
                                      bool destructive) const;

    // For the UI, translated.
    [[nodiscard]] static QString featureName(s3::Feature f);
    [[nodiscard]] static QString stateText(s3::CapabilityState state);
    [[nodiscard]] static QString sourceText(s3::CapabilitySource source);
    // Why an action needing `f` is blocked or unverified; empty if allowed.
    [[nodiscard]] static QString reason(s3::Feature f, const s3::CapabilityEntry& e, bool destructive);

signals:
    // Any update of an account's results (for pages showing them).
    void capabilitiesUpdated(const QUuid& account, const QString& bucket);
    // A feature's state changed (never on the first probe of a set).
    void changesDetected(const QUuid& account, const QString& bucket, const QList<s3::CapabilityChange>& changes);

private:
    struct Waiter {
        int maxAgeSecs = 0;
        std::function<void(bool)> done;
        QPointer<QObject> context;
    };
    struct BucketCaps {
        std::optional<s3::CapabilitySet> set;
        bool probing = false;
        QDateTime lastFailure;
        std::vector<Waiter> waiters;
    };
    struct AccountCaps {
        bool loading = false;
        bool loaded = false;
        std::optional<s3::CapabilitySet> server;
        bool probingServer = false;
        QDateTime serverFailure;
        std::map<QString, BucketCaps> buckets;
        QSet<QString> opened;   // buckets opened this session: kept fresh
        std::unique_ptr<QObject> run;   // context of this session's probes
    };

    void onSessionStarted_(const QUuid& account);
    void onSessionStopped_(const QUuid& account);
    void onAccountsChanged_();
    void onLearned_(const QUuid& account, const QString& bucket, const QString& featureId);
    void afterLoad_(const QUuid& account);
    void probeServer_(const QUuid& account);
    void probeBucket_(const QUuid& account, const QString& bucket);
    void apply_(const QUuid& account, const s3::CapabilitySet& fresh);
    void revalidate_();
    void finishWaiters_(BucketCaps& caps, bool fresh);
    [[nodiscard]] static bool youngerThan_(const std::optional<s3::CapabilitySet>& set, int secs);

    AccountStore* m_accounts;
    AccountSessions* m_sessions;
    CapabilityStore* m_store;
    ActivityLog* m_log;
    QHash<QUuid, std::shared_ptr<AccountCaps>> m_caps;
    QTimer m_timer;
};

} // namespace qbucket
