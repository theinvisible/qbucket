// SelfTest (`qbucket --self-test`): the environment lines, the keychain round
// trip with its own entry removed afterwards, the exit code per keychain
// answer, and the Flatpak detection.

#include "controllers/SelfTest.h"
#include "support/FakeKeychainBackend.h"

#include <QSignalSpy>
#include <QTextStream>
#include <QtTest>

using namespace qbucket;
using qbucket::testing::FakeKeychainBackend;

class TestSelfTest : public QObject {
    Q_OBJECT

private:
    // Runs the self-test to its end; returns the exit code (-1: no answer).
    static int run(FakeKeychainBackend* keychain, QString* output)
    {
        QTextStream out(output);
        SelfTest test(keychain, &out);
        QSignalSpy finished(&test, &SelfTest::finished);
        test.start();
        if (finished.isEmpty() && !finished.wait(5000))
            return -1;
        return finished.first().first().toInt();
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
    }

    void aWorkingKeychainPasses()
    {
        FakeKeychainBackend keychain;
        QString output;
        QCOMPARE(run(&keychain, &output), 0);
        QVERIFY2(output.contains(QStringLiteral("QBucket:    9.8.7")), qPrintable(output));
        QVERIFY(output.contains(QStringLiteral("Qt:         ") + QString::fromLatin1(qVersion())));
        QVERIFY(output.contains(QStringLiteral("Keychain:   ok (written, read back, removed)")));
        // Its own entry, and nothing left behind.
        QCOMPARE(keychain.writes, 1);
        QCOMPARE(keychain.reads, 1);
        QCOMPARE(keychain.removes, 1);
        QVERIFY(keychain.entries.isEmpty());
    }

    void noBackendFails()
    {
        FakeKeychainBackend keychain;
        keychain.mode = KeychainBackend::Status::NoBackend;
        QString output;
        QCOMPARE(run(&keychain, &output), 1);
        QVERIFY2(output.contains(QStringLiteral("Keychain:   write: no backend")), qPrintable(output));
        QCOMPARE(keychain.reads, 0);
    }

    void aFailingKeychainFails_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("text");
        QTest::newRow("denied") << int(KeychainBackend::Status::AccessDenied) << QStringLiteral("access denied");
        QTest::newRow("failed") << int(KeychainBackend::Status::Failed) << QStringLiteral("failed");
    }

    void aFailingKeychainFails()
    {
        QFETCH(int, mode);
        QFETCH(QString, text);
        FakeKeychainBackend keychain;
        keychain.mode = KeychainBackend::Status(mode);
        QString output;
        QCOMPARE(run(&keychain, &output), 1);
        QVERIFY2(output.contains(QStringLiteral("write: ") + text), qPrintable(output));
        // The keychain's own words go along, never a secret.
        QVERIFY(output.contains(QStringLiteral("fake keychain: unavailable")));
    }

    void aDifferentValueFails()
    {
        // A keychain that hands back something else than was written.
        class Lying final : public KeychainBackend {
        public:
            void read(const QString&, const QString&, ReadCallback done) override
            {
                QTimer::singleShot(1, [done] { done(Status::Ok, QStringLiteral("stale"), {}); });
            }
            void write(const QString&, const QString&, const QString&, DoneCallback done) override
            {
                QTimer::singleShot(1, [done] { done(Status::Ok, {}); });
            }
            void remove(const QString&, const QString&, DoneCallback done) override
            {
                ++removes;
                QTimer::singleShot(1, [done] { done(Status::Ok, {}); });
            }
            int removes = 0;
        } keychain;
        QString output;
        QTextStream out(&output);
        SelfTest test(&keychain, &out);
        QSignalSpy finished(&test, &SelfTest::finished);
        test.start();
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.first().first().toInt(), 1);
        QVERIFY2(output.contains(QStringLiteral("read: a different value came back")), qPrintable(output));
        QCOMPARE(keychain.removes, 1);   // cleaned up all the same
    }

    void detectsFlatpak()
    {
        const QByteArray before = qgetenv("FLATPAK_ID");
        qputenv("FLATPAK_ID", "io.github.theinvisible.QBucket");
        QVERIFY(SelfTest::inFlatpak());
        FakeKeychainBackend keychain;
        QString output;
        QCOMPARE(run(&keychain, &output), 0);
        QVERIFY2(output.contains(QStringLiteral("Sandbox:    Flatpak (io.github.theinvisible.QBucket)")),
                 qPrintable(output));
        if (before.isEmpty())
            qunsetenv("FLATPAK_ID");
        else
            qputenv("FLATPAK_ID", before);
        if (!QFile::exists(QStringLiteral("/.flatpak-info")))
            QVERIFY(!SelfTest::inFlatpak());
    }
};

QTEST_GUILESS_MAIN(TestSelfTest)
#include "test_self_test.moc"
