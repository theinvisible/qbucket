#include "accounts/LocationStore.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

Location place(const QUuid& account, const QString& bucket, const QString& prefix = {})
{
    return Location{account, bucket, prefix};
}

} // namespace

class TestLocationStore : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path() const { return m_dir.filePath(QStringLiteral("locations.json")); }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QFile::remove(path());
    }

    void bookmarksSurviveARestartInTheirOrder()
    {
        const QUuid a = QUuid::createUuid();
        {
            LocationStore store(path());
            QSignalSpy changed(&store, &LocationStore::bookmarksChanged);
            const Bookmark first = store.addBookmark(QString(), place(a, QStringLiteral("media"), QStringLiteral("photos/2026/")));
            QCOMPARE(first.name, QStringLiteral("2026"));   // the last segment
            store.addBookmark(QStringLiteral("  Backups "), place(a, QStringLiteral("pbs")));
            const Bookmark third = store.addBookmark(QString(), place(a, QStringLiteral("logs")));
            QCOMPARE(third.name, QStringLiteral("logs"));   // the bucket
            QVERIFY(store.moveBookmark(third.id, 0));
            QVERIFY(store.renameBookmark(first.id, QStringLiteral("Photos")));
            QVERIFY(!store.renameBookmark(first.id, QStringLiteral("  ")));
            QCOMPARE(changed.size(), 5);
        }
        LocationStore again(path());
        QStringList names;
        for (const Bookmark& b : again.bookmarks())
            names.append(b.name);
        QCOMPARE(names, (QStringList{QStringLiteral("logs"), QStringLiteral("Photos"), QStringLiteral("Backups")}));
        QCOMPARE(again.bookmarks().at(1).location, place(a, QStringLiteral("media"), QStringLiteral("photos/2026/")));
        QVERIFY(again.loadProblem().isEmpty());

        QVERIFY(again.removeBookmark(again.bookmarks().first().id));
        QCOMPARE(again.bookmarks().size(), 2);
        QVERIFY(!again.moveBookmark(QUuid::createUuid(), 0));
    }

    void recentIsDedupedBoundedAndDebounced()
    {
        const QUuid a = QUuid::createUuid();
        LocationStore store(path());
        store.visited(Location{a, {}, {}});   // an account alone: not a place
        QVERIFY(store.recent().isEmpty());
        for (int i = 0; i < LocationStore::kMaxRecent + 5; ++i)
            store.visited(place(a, QStringLiteral("b%1").arg(i)));
        QCOMPARE(store.recent().size(), LocationStore::kMaxRecent);
        QCOMPARE(store.recent().first().bucket, QStringLiteral("b%1").arg(LocationStore::kMaxRecent + 4));
        store.visited(place(a, QStringLiteral("b10")));
        QCOMPARE(store.recent().first().bucket, QStringLiteral("b10"));
        QCOMPARE(store.recent().count(place(a, QStringLiteral("b10"))), 1);

        // Not on disk yet (debounced) — flush() writes it.
        QVERIFY(LocationStore(path()).recent().isEmpty());
        store.flush();
        QCOMPARE(LocationStore(path()).recent().first().bucket, QStringLiteral("b10"));
        store.clearRecent();
        QVERIFY(LocationStore(path()).recent().isEmpty());
    }

    void forgettingAnAccountOrABucket()
    {
        const QUuid a = QUuid::createUuid();
        const QUuid b = QUuid::createUuid();
        LocationStore store(path());
        store.addBookmark(QString(), place(a, QStringLiteral("one")));
        store.addBookmark(QString(), place(a, QStringLiteral("two"), QStringLiteral("x/")));
        store.addBookmark(QString(), place(b, QStringLiteral("one")));
        store.visited(place(a, QStringLiteral("two"), QStringLiteral("x/")));
        store.visited(place(b, QStringLiteral("one")));
        store.forgetBucket(a, QStringLiteral("two"));
        QCOMPARE(store.bookmarks().size(), 2);
        QCOMPARE(store.recent().size(), 1);
        store.forgetAccount(b);
        QCOMPARE(store.bookmarks().size(), 1);
        QVERIFY(store.recent().isEmpty());
        QCOMPARE(LocationStore(path()).bookmarks().size(), 1);
    }

    void aDamagedFileStartsEmpty()
    {
        QFile f(path());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ not json");
        f.close();
        LocationStore store(path());
        QVERIFY(store.bookmarks().isEmpty());
        QVERIFY(!store.loadProblem().isEmpty());

        // Entries that make no sense are dropped, the rest stays.
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        const QByteArray account = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        f.write("{\"format\":1,\"bookmarks\":[{\"id\":\"" + id + "\",\"name\":\"ok\",\"account\":\"" + account
                + "\",\"bucket\":\"b\",\"prefix\":\"p/\"},{\"id\":\"\",\"name\":\"no id\"},{\"id\":\"" + id
                + "\",\"name\":\"twice\",\"account\":\"" + account + "\",\"bucket\":\"b\"}],"
                  "\"recent\":[{\"account\":\"" + account + "\",\"bucket\":\"b\",\"prefix\":\"key-not-folder\"}]}");
        f.close();
        LocationStore partly(path());
        QCOMPARE(partly.bookmarks().size(), 1);
        QCOMPARE(partly.bookmarks().first().name, QStringLiteral("ok"));
        QCOMPARE(partly.recent().size(), 1);
        QVERIFY(partly.recent().first().prefix.isEmpty());   // never a key as a folder
    }

    void defaultNames()
    {
        const QUuid a = QUuid::createUuid();
        QCOMPARE(LocationStore::defaultName(place(a, QStringLiteral("b"))), QStringLiteral("b"));
        QCOMPARE(LocationStore::defaultName(place(a, QStringLiteral("b"), QStringLiteral("x/y/"))), QStringLiteral("y"));
        QCOMPARE(LocationStore::defaultName(place(a, QStringLiteral("b"), QStringLiteral("x//"))), QStringLiteral("x"));
    }
};

QTEST_GUILESS_MAIN(TestLocationStore)
#include "test_location_store.moc"
