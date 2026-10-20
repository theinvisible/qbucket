#pragma once

#include <qbucket/s3/Capabilities.h>

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QUuid>

#include <functional>
#include <optional>

namespace qbucket {

// The capability results the app keeps between runs (PLAN.md §3.3), one
// directory per account under AppDataLocation/capabilities:
// server.json and bucket-<name>.json, each a CapabilitySet::toJson(), and
// history.jsonl, one line per detected change (kept to the newest
// kHistoryKeep lines).
// Expiry and merging are the CapabilityController's job; this only stores.
//
// File I/O runs on one background thread, in call order — a load after a
// save sees the saved state. Callbacks are queued to the context's thread
// and dropped if the context is gone. The destructor waits for pending
// writes (small files), so nothing saved is lost on quit.
//
// Thread affinity: GUI thread.
class CapabilityStore : public QObject {
    Q_OBJECT

public:
    struct Snapshot {
        std::optional<s3::CapabilitySet> server;
        QList<s3::CapabilitySet> buckets;
        QStringList problems;   // unreadable files, skipped (English, for the log)
    };
    using LoadCallback = std::function<void(const Snapshot&)>;
    using HistoryCallback = std::function<void(const QList<QJsonObject>&)>;

    static constexpr int kHistoryKeep = 1000;

    explicit CapabilityStore(QObject* parent = nullptr);
    // Explicit root directory, for tests.
    explicit CapabilityStore(const QString& rootDir, QObject* parent = nullptr);
    ~CapabilityStore() override;

    void load(const QUuid& account, QObject* context, LoadCallback done);
    void save(const QUuid& account, const s3::CapabilitySet& set);
    // When the account is deleted.
    void removeAll(const QUuid& account);

    void appendHistory(const QUuid& account, const QList<QJsonObject>& entries);
    // Oldest first.
    void loadHistory(const QUuid& account, QObject* context, HistoryCallback done);

    // Blocks until queued I/O is done — tests only.
    void waitForIdle();

    [[nodiscard]] QString rootDir() const { return m_root; }
    // "server.json", "bucket-<name>.json"; names that aren't plain lowercase
    // DNS labels are hex-encoded (case-insensitive file systems).
    [[nodiscard]] static QString fileNameFor(const s3::CapabilitySet& set);

private:
    [[nodiscard]] QString accountDir_(const QUuid& account) const;

    QString m_root;
    QThreadPool m_io;   // one thread: I/O stays in call order
};

} // namespace qbucket
