#pragma once

#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>

#include <functional>
#include <optional>

namespace qbucket::s3 {

struct RetryOptions {
    int maxAttempts = 4;          // including the first one
    int baseDelayMs = 250;
    int maxDelayMs = 20'000;
    // Uniform [0, 1); null: QRandomGenerator. Injectable for tests.
    std::function<double()> random;
};

// When to try a failed request again. Pure; thread-safe.
//
// Only transient failures (or ones a provider profile marked retryable) are
// retried, and only when that cannot do harm:
// idempotent requests always, others only when they provably never reached
// the server (connection refused, DNS, TLS handshake). A lost reply of a
// non-idempotent request (outcomeUnknown) is never retried — the caller
// resolves it (e.g. HEAD after CompleteMultipartUpload).
//
// Note: on versioned buckets a retried PUT/DELETE may leave one extra
// version or delete marker. That is the accepted price of safe retries.
class QBUCKET_S3_EXPORT RetryPolicy {
public:
    explicit RetryPolicy(RetryOptions options = {});

    [[nodiscard]] static bool isRetryable(const Error& error, bool idempotent);

    // Delay before attempt `failedAttempt + 1`, or nullopt to give up.
    // Exponential backoff with full jitter; a server Retry-After (seconds)
    // raises the delay, capped at 60 s.
    [[nodiscard]] std::optional<int> nextDelayMs(int failedAttempt, const Error& error,
                                                 bool idempotent,
                                                 std::optional<int> retryAfterSecs = {}) const;

    [[nodiscard]] const RetryOptions& options() const { return m_options; }

private:
    RetryOptions m_options;
};

} // namespace qbucket::s3
