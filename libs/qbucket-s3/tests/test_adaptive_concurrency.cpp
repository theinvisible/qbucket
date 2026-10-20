#include <qbucket/s3/transfer/AdaptiveConcurrency.h>

#include <QSignalSpy>
#include <QtTest>

#include <vector>

using namespace qbucket::s3::transfer;

class TestAdaptiveConcurrency : public QObject {
    Q_OBJECT

private:
    using Outcome = AdaptiveConcurrency::Outcome;
    using Slot = AdaptiveConcurrency::Slot;

    static std::vector<Slot> takeAll(AdaptiveConcurrency& c)
    {
        std::vector<Slot> out;
        while (const auto s = c.tryAcquire())
            out.push_back(*s);
        return out;
    }

private Q_SLOTS:
    void startsAtTheInitialLimit()
    {
        AdaptiveConcurrency c;
        QCOMPARE(c.limit(), 4);
        QCOMPARE(takeAll(c).size(), size_t(4));
        QCOMPARE(c.inFlight(), 4);
        QVERIFY(!c.tryAcquire());
    }

    void growsAfterAStreakOfSuccesses()
    {
        AdaptiveConcurrency c({2, 1, 4, 3});
        QSignalSpy grew(&c, &AdaptiveConcurrency::limitChanged);
        for (int i = 0; i < 3; ++i) {
            const auto s = c.tryAcquire();
            QVERIFY(s);
            c.release(*s, Outcome::Succeeded);
        }
        QCOMPARE(c.limit(), 3);
        QCOMPARE(grew.size(), 1);
        for (int i = 0; i < 30; ++i) {
            const auto s = c.tryAcquire();
            c.release(*s, Outcome::Succeeded);
        }
        QCOMPARE(c.limit(), 4);   // capped
    }

    void halvesOncePerWindow()
    {
        AdaptiveConcurrency c({8, 1, 16, 8});
        std::vector<Slot> slots = takeAll(c);
        QCOMPARE(slots.size(), size_t(8));
        // All eight in flight were throttled by the same overload: one halving.
        for (const Slot& s : slots)
            c.release(s, Outcome::Throttled);
        QCOMPARE(c.limit(), 4);
        // A request started after the decrease that is throttled halves again.
        const auto s = c.tryAcquire();
        c.release(*s, Outcome::Throttled);
        QCOMPARE(c.limit(), 2);
        for (int i = 0; i < 5; ++i) {
            const auto t = c.tryAcquire();
            c.release(*t, Outcome::Throttled);
        }
        QCOMPARE(c.limit(), 1);   // never below the minimum
    }

    void failuresSayNothingAboutLoad()
    {
        AdaptiveConcurrency c({4, 1, 16, 2});
        const auto a = c.tryAcquire();
        c.release(*a, Outcome::Succeeded);
        const auto b = c.tryAcquire();
        c.release(*b, Outcome::Failed);
        QCOMPARE(c.limit(), 4);
        const auto d = c.tryAcquire();
        c.release(*d, Outcome::Succeeded);
        QCOMPARE(c.limit(), 5);   // the streak survived the plain failure
    }

    void announcesFreeSlots()
    {
        AdaptiveConcurrency c({1, 1, 1, 8});
        QSignalSpy available(&c, &AdaptiveConcurrency::slotAvailable);
        const auto s = c.tryAcquire();
        QVERIFY(!c.tryAcquire());
        c.release(*s, Outcome::Succeeded);
        QCOMPARE(available.size(), 1);
        QVERIFY(c.tryAcquire());
    }

    void maximumCanChange()
    {
        AdaptiveConcurrency c({8, 1, 16, 1});
        c.setMaximum(3);
        QCOMPARE(c.limit(), 3);
        for (int i = 0; i < 10; ++i) {
            const auto s = c.tryAcquire();
            c.release(*s, Outcome::Succeeded);
        }
        QCOMPARE(c.limit(), 3);
        c.setMaximum(5);
        for (int i = 0; i < 10; ++i) {
            const auto s = c.tryAcquire();
            c.release(*s, Outcome::Succeeded);
        }
        QCOMPARE(c.limit(), 5);
        c.setMaximum(0);   // never below the minimum
        QCOMPARE(c.limit(), 1);
    }

    void sanitisesOptions()
    {
        AdaptiveConcurrency c({0, 0, -3, 0});
        QCOMPARE(c.limit(), 1);
        QCOMPARE(c.options().max, 1);
    }
};

QTEST_GUILESS_MAIN(TestAdaptiveConcurrency)
#include "test_adaptive_concurrency.moc"
