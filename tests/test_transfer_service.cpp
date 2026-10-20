#include "controllers/TransferService.h"

#include <qbucket/s3/transfer/ResumeStore.h>

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

class TestTransferService : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void oneLimiterPerAccount()
    {
        QTemporaryDir dir;
        TransferService t(dir.path(), nullptr);
        const QUuid a = QUuid::createUuid();
        const QUuid b = QUuid::createUuid();
        QCOMPARE(t.limiter(a), t.limiter(a));   // shared by every transfer of the account
        QVERIFY(t.limiter(a) != t.limiter(b));
        QCOMPARE(t.limiter(a)->options().max, 16);
    }

    void theCeilingFollowsTheSettings()
    {
        QTemporaryDir dir;
        TransferService t(dir.path(), nullptr);
        const QUuid a = QUuid::createUuid();
        t.setMaxParallelParts(2);
        QCOMPARE(t.limiter(a)->limit(), 2);   // new limiters start below it
        t.setMaxParallelParts(1);
        QCOMPARE(t.limiter(a)->limit(), 1);
        QCOMPARE(t.limiter(QUuid::createUuid())->options().max, 1);
    }

    void storeLivesUnderTheDirectory()
    {
        QTemporaryDir dir;
        TransferService t(dir.path(), nullptr);
        s3::transfer::TransferRecord r;
        r.id = QStringLiteral("x1");
        QVERIFY(t.resumeStore()->begin(r));
        QCOMPARE(t.resumeStore()->ids(), QStringList{QStringLiteral("x1")});
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("parts/x1.json"))));
        QCOMPARE(t.jobDirectory(), dir.filePath(QStringLiteral("jobs")));

        // The default: non-roaming application data.
        TransferService d;
        QVERIFY(d.directory().startsWith(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)));
    }
};

QTEST_GUILESS_MAIN(TestTransferService)
#include "test_transfer_service.moc"
