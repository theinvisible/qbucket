#include <qbucket/s3/transfer/AdaptiveConcurrency.h>

#include <algorithm>

namespace qbucket::s3::transfer {

AdaptiveConcurrency::AdaptiveConcurrency(QObject* parent)
    : AdaptiveConcurrency(Options(), parent)
{
}

AdaptiveConcurrency::AdaptiveConcurrency(Options options, QObject* parent)
    : QObject(parent)
    , m_options(options)
{
    m_options.min = std::max(1, m_options.min);
    m_options.max = std::max(m_options.min, m_options.max);
    m_options.increaseAfter = std::max(1, m_options.increaseAfter);
    m_limit = std::clamp(m_options.initial, m_options.min, m_options.max);
}

std::optional<AdaptiveConcurrency::Slot> AdaptiveConcurrency::tryAcquire()
{
    if (m_inFlight >= m_limit)
        return std::nullopt;
    ++m_inFlight;
    return Slot{m_window};
}

void AdaptiveConcurrency::release(Slot slot, Outcome outcome)
{
    Q_ASSERT(m_inFlight > 0);
    m_inFlight = std::max(0, m_inFlight - 1);
    const int before = m_limit;
    switch (outcome) {
    case Outcome::Throttled:
        m_streak = 0;
        // One decrease per window: the requests that were in flight with
        // this one were throttled by the same overload.
        if (slot.window == m_window) {
            m_limit = std::max(m_options.min, m_limit / 2);
            ++m_window;
        }
        break;
    case Outcome::Succeeded:
        if (++m_streak >= m_options.increaseAfter && m_limit < m_options.max) {
            ++m_limit;
            m_streak = 0;
        }
        break;
    case Outcome::Failed:
        break;
    }
    if (m_limit != before)
        Q_EMIT limitChanged(m_limit);
    if (m_inFlight < m_limit)
        Q_EMIT slotAvailable();
}

} // namespace qbucket::s3::transfer
