#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace qbucket {

// ---------------------------------------------------------------------------
// StallWatchdog — reports GUI-thread event-loop stalls.
// ---------------------------------------------------------------------------
// "The UI never blocks" is a hard rule (PLAN.md §3, threading model). This is
// how we notice when it is broken during development: a precise heartbeat
// timer on the watched thread measures how late each tick arrives. A tick
// that is late by more than the threshold means the event loop was busy —
// something ran on the GUI thread that should have been on a worker.
//
// Detection is after the fact (the stall is reported once the loop runs
// again), which is exactly what we need for development and benchmarks.
// Enabled by default in debug builds; QBUCKET_STALL_WATCHDOG=1/0 overrides.
// ---------------------------------------------------------------------------
class StallWatchdog : public QObject {
    Q_OBJECT

public:
    explicit StallWatchdog(int thresholdMs = 50, QObject* parent = nullptr);

    void start();
    void stop();
    bool isRunning() const { return m_timer.isActive(); }

    int     thresholdMs()  const { return m_thresholdMs; }
    int     stallCount()   const { return m_stallCount; }
    qint64  longestStallMs() const { return m_longestStallMs; }

    // Debug build → on, release → off; the environment variable
    // QBUCKET_STALL_WATCHDOG ("1"/"0") wins over both.
    static bool enabledByDefault();

signals:
    void stallDetected(qint64 durationMs);

private:
    void tick_();

    static constexpr int kIntervalMs = 20;

    QTimer        m_timer;
    QElapsedTimer m_clock;
    int           m_thresholdMs;
    int           m_stallCount = 0;
    qint64        m_longestStallMs = 0;
};

} // namespace qbucket
