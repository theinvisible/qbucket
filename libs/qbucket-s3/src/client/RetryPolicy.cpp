#include "client/RetryPolicy.h"

#include <QRandomGenerator>

#include <algorithm>

namespace qbucket::s3 {

RetryPolicy::RetryPolicy(RetryOptions options)
    : m_options(std::move(options))
{
}

bool RetryPolicy::isRetryable(const Error& error, bool idempotent)
{
    if (error.category != ErrorCategory::Transient)
        return false;
    if (idempotent)
        return true;
    // Never reached the server: no response, and the outcome is not in doubt.
    return error.httpStatus == 0 && !error.outcomeUnknown;
}

std::optional<int> RetryPolicy::nextDelayMs(int failedAttempt, const Error& error, bool idempotent,
                                            std::optional<int> retryAfterSecs) const
{
    if (failedAttempt >= m_options.maxAttempts || !isRetryable(error, idempotent))
        return std::nullopt;

    const int shift = std::min(failedAttempt - 1, 16);
    const qint64 ceiling = std::min<qint64>(m_options.maxDelayMs,
                                            qint64(m_options.baseDelayMs) << shift);
    const double r = m_options.random ? m_options.random() : QRandomGenerator::global()->generateDouble();
    qint64 delay = qint64(r * double(ceiling));
    if (retryAfterSecs && *retryAfterSecs > 0)
        delay = std::max<qint64>(delay, qint64(std::min(*retryAfterSecs, 60)) * 1000);
    return int(delay);
}

} // namespace qbucket::s3
