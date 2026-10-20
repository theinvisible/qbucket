#include "client/RetryPolicy.h"

#include <QtTest>

using namespace qbucket::s3;

namespace {

Error transient(int status = 503, bool outcomeUnknown = false)
{
    Error e = Error::make(ErrorCategory::Transient, QStringLiteral("x"));
    e.httpStatus = status;
    e.outcomeUnknown = outcomeUnknown;
    return e;
}

RetryOptions fixedRandom(double r)
{
    RetryOptions o;
    o.maxAttempts = 4;
    o.baseDelayMs = 100;
    o.maxDelayMs = 1000;
    o.random = [r] { return r; };
    return o;
}

} // namespace

class TestRetryPolicy : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void whatIsRetryable()
    {
        QVERIFY(RetryPolicy::isRetryable(transient(), true));
        QVERIFY(RetryPolicy::isRetryable(transient(0), true));
        // Non-idempotent: only when it never reached the server.
        QVERIFY(RetryPolicy::isRetryable(transient(0), false));
        QVERIFY(!RetryPolicy::isRetryable(transient(0, true), false));
        QVERIFY(!RetryPolicy::isRetryable(transient(503), false));
        // Never non-transient categories.
        for (const ErrorCategory c : {ErrorCategory::NotFound, ErrorCategory::NotPermitted,
                                      ErrorCategory::Unsupported, ErrorCategory::Protocol,
                                      ErrorCategory::Cancelled, ErrorCategory::Authentication}) {
            QVERIFY(!RetryPolicy::isRetryable(Error::make(c, QString()), true));
        }
    }

    void exponentialBackoffWithFullJitter()
    {
        const RetryPolicy max(fixedRandom(0.999999));
        QCOMPARE(max.nextDelayMs(1, transient(), true).value(), 99);
        QCOMPARE(max.nextDelayMs(2, transient(), true).value(), 199);
        QCOMPARE(max.nextDelayMs(3, transient(), true).value(), 399);
        QVERIFY(!max.nextDelayMs(4, transient(), true));   // attempts exhausted

        const RetryPolicy zero(fixedRandom(0.0));
        QCOMPARE(zero.nextDelayMs(3, transient(), true).value(), 0);

        RetryOptions capped = fixedRandom(0.999999);
        capped.maxAttempts = 30;
        QCOMPARE(RetryPolicy(capped).nextDelayMs(20, transient(), true).value(), 999);
    }

    void retryAfterRaisesTheDelay()
    {
        const RetryPolicy p(fixedRandom(0.0));
        QCOMPARE(p.nextDelayMs(1, transient(), true, 2).value(), 2000);
        QCOMPARE(p.nextDelayMs(1, transient(), true, 3600).value(), 60000);   // capped
        QCOMPARE(p.nextDelayMs(1, transient(), true, 0).value(), 0);
    }

    void defaultRandomStaysInRange()
    {
        const RetryPolicy p;
        for (int i = 0; i < 200; ++i) {
            const int d = p.nextDelayMs(2, transient(), true).value();
            QVERIFY(d >= 0 && d < 500);
        }
    }
};

QTEST_GUILESS_MAIN(TestRetryPolicy)
#include "test_retry_policy.moc"
