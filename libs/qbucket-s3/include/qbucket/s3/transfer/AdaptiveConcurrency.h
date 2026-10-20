#pragma once

#include <qbucket/s3/transfer/Export.h>

#include <QObject>

#include <optional>

namespace qbucket::s3::transfer {

// AIMD limit for parallel transfer requests to one endpoint (PLAN.md §13):
// one more slot after a run of unthrottled successes, half the slots when
// the server throttles (SlowDown / 503) — once per window: throttles of
// requests that started before the last decrease don't halve again.
//
// Shared by every transfer of one account. Thread affinity: lives on one
// thread (usually the GUI thread, where the transfers are coordinated); all
// calls happen there.
class QBUCKET_S3_TRANSFER_EXPORT AdaptiveConcurrency : public QObject {
    Q_OBJECT

public:
    struct Options {
        int initial = 4;
        int min = 1;
        int max = 16;
        int increaseAfter = 8;   // unthrottled successes per extra slot
    };

    // A held slot; give it back with release().
    struct Slot {
        quint64 window = 0;
    };

    enum class Outcome : quint8 {
        Succeeded,   // and not throttled
        Throttled,   // throttled on the way, succeeded or not
        Failed,      // any other failure — no signal about load
    };

    explicit AdaptiveConcurrency(QObject* parent = nullptr);
    explicit AdaptiveConcurrency(Options options, QObject* parent = nullptr);

    [[nodiscard]] int limit() const { return m_limit; }
    [[nodiscard]] int inFlight() const { return m_inFlight; }
    [[nodiscard]] const Options& options() const { return m_options; }

    // A new ceiling (e.g. from the settings); the limit follows it down at
    // once and grows to it as usual.
    void setMaximum(int max);

    // A slot if fewer than limit() are taken.
    [[nodiscard]] std::optional<Slot> tryAcquire();
    void release(Slot slot, Outcome outcome);

Q_SIGNALS:
    // A slot became free (or the limit grew): waiting transfers try again.
    void slotAvailable();
    void limitChanged(int limit);

private:
    Options m_options;
    int m_limit = 4;
    int m_inFlight = 0;
    int m_streak = 0;
    quint64 m_window = 0;
};

} // namespace qbucket::s3::transfer
