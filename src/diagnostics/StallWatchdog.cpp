#include "diagnostics/StallWatchdog.h"

#include "diagnostics/Logging.h"

namespace qbucket {

StallWatchdog::StallWatchdog(int thresholdMs, QObject* parent)
    : QObject(parent),
      m_thresholdMs(thresholdMs)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &StallWatchdog::tick_);
}

void StallWatchdog::start()
{
    m_clock.start();
    m_timer.start();
}

void StallWatchdog::stop()
{
    m_timer.stop();
}

bool StallWatchdog::enabledByDefault()
{
    bool ok = false;
    const int env = qEnvironmentVariableIntValue("QBUCKET_STALL_WATCHDOG", &ok);
    if (ok)
        return env != 0;
#ifdef QT_DEBUG
    return true;
#else
    return false;
#endif
}

void StallWatchdog::tick_()
{
    // Time since the previous tick minus the interval we asked for is how
    // long the event loop kept this timer waiting.
    const qint64 elapsed = m_clock.restart();
    const qint64 late = elapsed - kIntervalMs;
    if (late <= m_thresholdMs)
        return;

    ++m_stallCount;
    if (late > m_longestStallMs)
        m_longestStallMs = late;

    qCWarning(lcStall).noquote()
        << QStringLiteral("GUI thread stalled for %1 ms (threshold %2 ms)")
               .arg(late).arg(m_thresholdMs);
    emit stallDetected(late);
}

} // namespace qbucket
