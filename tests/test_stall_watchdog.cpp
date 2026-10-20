#include "diagnostics/StallWatchdog.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

using namespace qbucket;

class TestStallWatchdog : public QObject {
    Q_OBJECT

private slots:
    void detectsABlockedEventLoop()
    {
        StallWatchdog watchdog(50);
        QSignalSpy spy(&watchdog, &StallWatchdog::stallDetected);
        watchdog.start();

        // Let the heartbeat settle, then block the event loop for ~200 ms.
        QTest::qWait(60);
        QTimer::singleShot(0, this, [] { QThread::msleep(200); });
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 2000);

        QVERIFY(watchdog.stallCount() >= 1);
        QVERIFY(watchdog.longestStallMs() >= 100);
        QVERIFY(spy.first().first().toLongLong() >= 100);
    }

    void quietEventLoopReportsNothing()
    {
        // Generous threshold so a busy CI machine doesn't produce a false
        // positive; what matters is that normal idling is not a stall.
        StallWatchdog watchdog(500);
        QSignalSpy spy(&watchdog, &StallWatchdog::stallDetected);
        watchdog.start();
        QTest::qWait(300);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(watchdog.stallCount(), 0);
    }

    void stopsCleanly()
    {
        StallWatchdog watchdog;
        watchdog.start();
        QVERIFY(watchdog.isRunning());
        watchdog.stop();
        QVERIFY(!watchdog.isRunning());
    }

    void environmentOverridesDefault()
    {
        qputenv("QBUCKET_STALL_WATCHDOG", "0");
        QVERIFY(!StallWatchdog::enabledByDefault());
        qputenv("QBUCKET_STALL_WATCHDOG", "1");
        QVERIFY(StallWatchdog::enabledByDefault());
        qunsetenv("QBUCKET_STALL_WATCHDOG");
    }
};

QTEST_GUILESS_MAIN(TestStallWatchdog)
#include "test_stall_watchdog.moc"
