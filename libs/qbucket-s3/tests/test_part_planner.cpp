#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/transfer/PartPlanner.h>

#include <QtTest>

using namespace qbucket::s3;
using namespace qbucket::s3::transfer;

namespace {

constexpr qint64 kMiB = Q_INT64_C(1024) * 1024;
constexpr qint64 kGiB = 1024 * kMiB;

// A provider with tighter limits.
class SmallProfile final : public GenericS3Profile {
public:
    [[nodiscard]] qint64 minPartSize() const override { return 16 * kMiB; }
    [[nodiscard]] qint64 maxPartSize() const override { return 1 * kGiB; }
    [[nodiscard]] int maxParts() const override { return 100; }
};

} // namespace

class TestPartPlanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void belowTheThresholdIsOneRequest()
    {
        const GenericS3Profile p;
        for (const qint64 size : {qint64(0), qint64(1), 64 * kMiB - 1}) {
            const auto plan = planParts(size, p);
            QVERIFY(plan);
            QVERIFY(!plan->multipart);
            QCOMPARE(plan->partCount, 1);
            QCOMPARE(plan->partSize, size);
            QCOMPARE(plan->partLength(1), size);
        }
    }

    void fromTheThresholdOnEightMiBParts()
    {
        const auto plan = planParts(64 * kMiB, GenericS3Profile());
        QVERIFY(plan);
        QVERIFY(plan->multipart);
        QCOMPARE(plan->partSize, 8 * kMiB);
        QCOMPARE(plan->partCount, 8);
        QCOMPARE(plan->partLength(8), 8 * kMiB);

        const auto odd = planParts(100 * kMiB + 5, GenericS3Profile());
        QVERIFY(odd);
        QCOMPARE(odd->partCount, 13);
        QCOMPARE(odd->partOffset(13), 96 * kMiB);
        QCOMPARE(odd->partLength(13), 4 * kMiB + 5);
    }

    void largeFilesStayWithinTenThousandParts()
    {
        // 8 MiB × 10000 = 78.1 GiB; above that the parts grow in whole MiB.
        const auto at = planParts(8 * kMiB * 10000, GenericS3Profile());
        QVERIFY(at);
        QCOMPARE(at->partSize, 8 * kMiB);
        QCOMPARE(at->partCount, 10000);

        const auto above = planParts(8 * kMiB * 10000 + 1, GenericS3Profile());
        QVERIFY(above);
        QCOMPARE(above->partSize, 9 * kMiB);
        QVERIFY(above->partCount <= 10000);

        const qint64 fiftyGb = Q_INT64_C(50) * 1000 * 1000 * 1000;
        const auto fifty = planParts(fiftyGb, GenericS3Profile());
        QVERIFY(fifty);
        QCOMPARE(fifty->partSize, 8 * kMiB);
        QCOMPARE(fifty->partCount, 5961);

        const auto max = planParts(kMaxObjectSize, GenericS3Profile());
        QVERIFY(max);
        QVERIFY(max->partCount <= 10000);
        QVERIFY(max->partSize <= 5 * kGiB);
        QCOMPARE(max->partSize % kMiB, 0);
        QVERIFY(max->partSize * qint64(max->partCount) >= kMaxObjectSize);
    }

    void refusesWhatS3Cannot()
    {
        QVERIFY(!planParts(-1, GenericS3Profile()));
        const auto tooBig = planParts(kMaxObjectSize + 1, GenericS3Profile());
        QVERIFY(!tooBig);
        QCOMPARE(tooBig.error().category, ErrorCategory::InvalidRequest);
    }

    void followsTheProfile()
    {
        const SmallProfile p;
        const auto plan = planParts(100 * kMiB, p);
        QVERIFY(plan);
        QCOMPARE(plan->partSize, 16 * kMiB);   // the provider's minimum wins

        const auto many = planParts(10 * kGiB, p);   // 100 parts → 103 MiB each
        QVERIFY(many);
        QVERIFY(many->partCount <= 100);
        QCOMPARE(many->partSize, 103 * kMiB);

        QVERIFY(!planParts(101 * kGiB, p));   // would need parts above 1 GiB
    }

    void optionsMove()
    {
        PartPlannerOptions o;
        o.multipartThreshold = 10 * kMiB;
        o.minPartSize = 5 * kMiB;
        const auto plan = planParts(12 * kMiB, GenericS3Profile(), o);
        QVERIFY(plan);
        QVERIFY(plan->multipart);
        QCOMPARE(plan->partCount, 3);
        // A single PUT never exceeds the maximum part size.
        o.multipartThreshold = 100 * kGiB;
        const auto capped = planParts(6 * kGiB, GenericS3Profile(), o);
        QVERIFY(capped);
        QVERIFY(capped->multipart);
    }
};

QTEST_GUILESS_MAIN(TestPartPlanner)
#include "test_part_planner.moc"
