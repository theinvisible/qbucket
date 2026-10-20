#include "accounts/CapabilityStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

s3::CapabilitySet serverSet()
{
    s3::CapabilitySet set;
    set.probedAt = QDateTime(QDate(2026, 10, 20), QTime(8, 0), QTimeZone::UTC);
    set.fingerprint.serverHeader = QStringLiteral("Ceph Object Gateway (squid)");
    set.set(s3::Feature::Cors, {s3::CapabilityState::Supported, s3::CapabilitySource::Probed,
                                set.probedAt, 404, QStringLiteral("NoSuchCORSConfiguration"), false, 0});
    return set;
}

s3::CapabilitySet bucketSet(const QString& name)
{
    s3::CapabilitySet set;
    set.scope = s3::CapabilityScope::Bucket;
    set.bucket = name;
    set.probedAt = QDateTime(QDate(2026, 10, 20), QTime(8, 5), QTimeZone::UTC);
    set.bucketState.versioning = QStringLiteral("Enabled");
    return set;
}

CapabilityStore::Snapshot loadNow(CapabilityStore& store, const QUuid& account)
{
    QObject context;
    std::optional<CapabilityStore::Snapshot> out;
    store.load(account, &context, [&](const CapabilityStore::Snapshot& s) { out = s; });
    if (!QTest::qWaitFor([&] { return out.has_value(); }, 5000))
        return {};
    return *out;
}

} // namespace

class TestCapabilityStore : public QObject {
    Q_OBJECT

private slots:
    void fileNames()
    {
        QCOMPARE(CapabilityStore::fileNameFor(serverSet()), QStringLiteral("server.json"));
        QCOMPARE(CapabilityStore::fileNameFor(bucketSet(QStringLiteral("pbs-backup.v2"))),
                 QStringLiteral("bucket-pbs-backup.v2.json"));
        // Legacy names with capitals would collide on Windows/macOS.
        QCOMPARE(CapabilityStore::fileNameFor(bucketSet(QStringLiteral("Media"))),
                 QStringLiteral("bucket-x-4d65646961.json"));
        QVERIFY(CapabilityStore::fileNameFor(bucketSet(QStringLiteral("a/../b"))).startsWith(QLatin1String("bucket-x-")));
    }

    void roundTripPerAccount()
    {
        QTemporaryDir dir;
        const QUuid a = QUuid::createUuid();
        const QUuid b = QUuid::createUuid();
        {
            CapabilityStore store(dir.path());
            store.save(a, serverSet());
            store.save(a, bucketSet(QStringLiteral("one")));
            store.save(a, bucketSet(QStringLiteral("Two")));
            store.save(b, bucketSet(QStringLiteral("other")));
        }   // the destructor waits for the writes

        CapabilityStore store(dir.path());
        const CapabilityStore::Snapshot snap = loadNow(store, a);
        QVERIFY(snap.problems.isEmpty());
        QVERIFY(snap.server);
        QCOMPARE(snap.server->fingerprint.serverHeader, QStringLiteral("Ceph Object Gateway (squid)"));
        QCOMPARE(snap.server->state(s3::Feature::Cors), s3::CapabilityState::Supported);
        QCOMPARE(snap.buckets.size(), 2);
        QStringList names;
        for (const s3::CapabilitySet& s : snap.buckets)
            names.append(s.bucket);
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("Two"), QStringLiteral("one")}));
        QCOMPARE(snap.buckets.first().bucketState.versioning, QStringLiteral("Enabled"));

        QCOMPARE(loadNow(store, b).buckets.size(), 1);
        QVERIFY(!loadNow(store, QUuid::createUuid()).server);
    }

    void loadSeesEarlierSaves()
    {
        QTemporaryDir dir;
        CapabilityStore store(dir.path());
        const QUuid a = QUuid::createUuid();
        store.save(a, serverSet());
        QVERIFY(loadNow(store, a).server);   // same queue, no waitForIdle needed
    }

    void corruptFilesAreSkippedAndReported()
    {
        QTemporaryDir dir;
        const QUuid a = QUuid::createUuid();
        CapabilityStore store(dir.path());
        store.save(a, bucketSet(QStringLiteral("good")));
        store.waitForIdle();
        const QString accountDir = dir.path() + u'/' + a.toString(QUuid::WithoutBraces);
        QFile broken(accountDir + QStringLiteral("/server.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();
        // A bucket file holding a server-scope set.
        QFile wrongScope(accountDir + QStringLiteral("/bucket-odd.json"));
        QVERIFY(wrongScope.open(QIODevice::WriteOnly));
        wrongScope.write(QJsonDocument(serverSet().toJson()).toJson());
        wrongScope.close();

        const CapabilityStore::Snapshot snap = loadNow(store, a);
        QVERIFY(!snap.server);
        QCOMPARE(snap.buckets.size(), 1);
        QCOMPARE(snap.problems.size(), 2);
    }

    void removeAllForgetsTheAccount()
    {
        QTemporaryDir dir;
        const QUuid a = QUuid::createUuid();
        CapabilityStore store(dir.path());
        store.save(a, serverSet());
        store.removeAll(a);
        store.waitForIdle();
        QVERIFY(!QDir(dir.path() + u'/' + a.toString(QUuid::WithoutBraces)).exists());
        QVERIFY(!loadNow(store, a).server);
    }

    void callbackDroppedWithItsContext()
    {
        QTemporaryDir dir;
        CapabilityStore store(dir.path());
        bool called = false;
        {
            QObject context;
            store.load(QUuid::createUuid(), &context, [&](const CapabilityStore::Snapshot&) { called = true; });
        }
        store.waitForIdle();
        QTest::qWait(50);
        QVERIFY(!called);
    }
};

QTEST_GUILESS_MAIN(TestCapabilityStore)
#include "test_capability_store.moc"
