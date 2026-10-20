#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/ObjectTableModel.h"
#include "support/FakeBucket.h"
#include "support/FakeKeychainBackend.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::FakeBucket;

namespace {

const QString kKeyId = QStringLiteral("AKIDOBJECTS");
const QString kSecret = QStringLiteral("objects-secret");

using State = ObjectTableModel::State;

QString name(const ObjectTableModel& m, int row)
{
    return m.index(row, ObjectTableModel::NameColumn).data().toString();
}

bool isFolder(const ObjectTableModel& m, int row)
{
    return m.index(row, 0).data(ObjectTableModel::IsFolderRole).toBool();
}

QString key(int i)
{
    return QStringLiteral("f%1.txt").arg(i, 5, 10, QLatin1Char('0'));
}

} // namespace

class TestObjectTableModel : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_server = std::make_unique<FakeS3Server>();
        QVERIFY(m_server->listen());
        s3::Credentials c;
        c.accessKeyId = kKeyId;
        c.secretAccessKey = kSecret;
        m_server->setVerifySignatures(c, "test-1");
        m_bucket = FakeBucket();
        m_server->setFallbackHandler([this](const RecordedRequest& req) { return m_bucket.list(req); });

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run)));
        auto backend = std::make_unique<qbucket::testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("local");
        a.providerId = QStringLiteral("generic");
        a.region = QStringLiteral("test-1");
        a.endpoint = m_server->endpointUrl().toString();
        a.accessKeyId = kKeyId;
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                kSecret);
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        QVERIFY(m_accounts->save(a));
        m_account = a.id;
        m_sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
    }

    void cleanup()
    {
        m_sessions.reset();
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void pagesOneThousandAtATime()
    {
        for (int i = 0; i < 2500; ++i)
            m_bucket.add(key(i), i);
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        QSignalSpy pages(&model, &ObjectTableModel::pageLoaded);
        QCOMPARE(model.state(), State::Idle);
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QCOMPARE(model.state(), State::Loading);
        QCOMPARE(model.rowCount(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1000, 10000);
        QCOMPARE(model.state(), State::Ready);
        QVERIFY(model.canLoadMore());
        QCOMPARE(name(model, 0), key(0));
        QCOMPARE(model.index(5, ObjectTableModel::SizeColumn).data(ObjectTableModel::SizeRole).toLongLong(), 5);

        model.loadMore();
        QVERIFY(!model.canLoadMore());   // one page at a time
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2000, 10000);
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 2500);
        QVERIFY(!model.canLoadMore());
        QCOMPARE(model.pagesLoaded(), 3);
        QCOMPARE(pages.size(), 3);
        QCOMPARE(model.loadedObjects(), 2500);
        QCOMPARE(model.loadedBytes(), qint64(2499) * 2500 / 2);
        QCOMPARE(m_server->requestCount(), 3);
        const RecordedRequest first = m_server->requests().first();
        QCOMPARE(first.queryItem("delimiter"), QByteArray("%2F"));
        QCOMPARE(first.queryItem("max-keys"), QByteArray("1000"));
    }

    void foldersFirstAcrossPages()
    {
        m_bucket.add(QStringLiteral("a/1"));
        for (int i = 0; i < 1200; ++i)
            m_bucket.add(key(i));
        m_bucket.add(QStringLiteral("zz/1"));   // a common prefix of the second page
        m_bucket.add(QStringLiteral("zz/2"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());   // (the model tester is too slow for 1000s of rows)
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        // A common prefix counts against max-keys: "a" + 999 objects.
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1000, 10000);
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 1202);
        QCOMPARE(name(model, 0), QStringLiteral("a"));
        QVERIFY(isFolder(model, 0));
        QCOMPARE(name(model, 1), QStringLiteral("zz"));
        QVERIFY(isFolder(model, 1));
        QCOMPARE(model.index(1, 0).data(ObjectTableModel::KeyRole).toString(), QStringLiteral("zz/"));
        QCOMPARE(name(model, 2), key(0));
        QCOMPARE(model.loadedFolders(), 2);
        QCOMPARE(model.index(0, ObjectTableModel::SizeColumn).data().toString(), QStringLiteral("—"));
    }

    void namesAreRelativeAndTheMarkerIsHidden()
    {
        m_bucket.add(QStringLiteral("photos/"));
        m_bucket.add(QStringLiteral("photos/2026/a.jpg"));
        m_bucket.add(QStringLiteral("photos/b.jpg"), 10, "GLACIER");
        m_bucket.add(QStringLiteral("other.txt"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("photos/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(name(model, 0), QStringLiteral("2026"));
        QCOMPARE(name(model, 1), QStringLiteral("b.jpg"));
        QCOMPARE(model.index(1, 0).data(ObjectTableModel::KeyRole).toString(), QStringLiteral("photos/b.jpg"));
        QCOMPARE(model.index(1, ObjectTableModel::StorageClassColumn).data().toString(), QStringLiteral("GLACIER"));
        QCOMPARE(model.index(1, 0).data(Qt::ToolTipRole).toString(), QStringLiteral("photos/b.jpg"));
    }

    void flatModeListsEverythingBelow()
    {
        m_bucket.add(QStringLiteral("photos/2026/a.jpg"));
        m_bucket.add(QStringLiteral("photos/b.jpg"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("photos/"), ObjectTableModel::Mode::Flat);
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(name(model, 0), QStringLiteral("2026/a.jpg"));
        QVERIFY(!isFolder(model, 0));
        QVERIFY(!m_server->requests().first().hasQueryItem("delimiter")
                || m_server->requests().first().queryItem("delimiter").isEmpty());
    }

    void anErrorKeepsTheRowsAndSaysSo()
    {
        for (int i = 0; i < 1500; ++i)
            m_bucket.add(key(i));
        m_bucket.sort();
        m_server->setFallbackHandler([this](const RecordedRequest& req) {
            if (req.hasQueryItem("continuation-token"))
                return FakeResponse::s3Error(403, "AccessDenied", "no more for you");
            return m_bucket.list(req);
        });
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1000, 10000);
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Error, 10000);
        QCOMPARE(model.rowCount(), 1000);
        QCOMPARE(model.error().category, s3::ErrorCategory::NotPermitted);
        QVERIFY(!model.errorText().isEmpty());
        QVERIFY(!model.canLoadMore());
    }

    void aFailedFirstPageIsAnErrorNotAnEmptyFolder()
    {
        m_server->setFallbackHandler([](const RecordedRequest&) { return FakeResponse::s3Error(404, "NoSuchBucket"); });
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("gone"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Error, 10000);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.error().category, s3::ErrorCategory::NotFound);
    }

    void aRepeatedTokenStopsTheListing()
    {
        for (int i = 0; i < 1500; ++i)
            m_bucket.add(key(i));
        m_bucket.sort();
        m_server->setFallbackHandler([this](const RecordedRequest& req) {
            FakeResponse r = m_bucket.list(req);
            r.body.replace("<NextContinuationToken>1000</NextContinuationToken>",
                           "<NextContinuationToken>same</NextContinuationToken>");
            if (req.hasQueryItem("continuation-token"))
                r = FakeResponse::ok("<ListBucketResult><IsTruncated>true</IsTruncated>"
                                     "<NextContinuationToken>same</NextContinuationToken></ListBucketResult>");
            return r;
        });
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1000, 10000);
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Error, 10000);
        QCOMPARE(model.error().category, s3::ErrorCategory::Protocol);
    }

    void navigatingAwayDropsTheOldListing()
    {
        m_bucket.add(QStringLiteral("old/x"));
        m_bucket.add(QStringLiteral("new/y"));
        m_bucket.sort();
        m_server->setFallbackHandler([this](const RecordedRequest& req) {
            FakeResponse r = m_bucket.list(req);
            if (QUrl::fromPercentEncoding(req.queryItem("prefix")) == QLatin1String("old/"))
                r.delayMs = 500;
            return r;
        });
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("old/"));
        QTRY_COMPARE(m_server->requestCount(), 1);
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("new/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QTest::qWait(700);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(name(model, 0), QStringLiteral("y"));
    }

    void smallSortsRunInline()
    {
        m_bucket.add(QStringLiteral("dir/x"));
        m_bucket.add(QStringLiteral("file10.txt"), 5);
        m_bucket.add(QStringLiteral("file9.txt"), 50);
        m_bucket.add(QStringLiteral("File1.txt"), 500);
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        // Server order: bytes ("File1" < "dir" < "file10" < "file9"), folders first.
        QCOMPARE(name(model, 0), QStringLiteral("dir"));
        QCOMPARE(name(model, 1), QStringLiteral("File1.txt"));
        QCOMPARE(name(model, 2), QStringLiteral("file10.txt"));

        const QPersistentModelIndex nine = model.index(3, 0);
        QCOMPARE(nine.data().toString(), QStringLiteral("file9.txt"));
        model.sort(ObjectTableModel::NameColumn, Qt::AscendingOrder);
        QVERIFY(!model.isSorting());
        QCOMPARE(name(model, 0), QStringLiteral("dir"));        // folders stay first
        QCOMPARE(name(model, 1), QStringLiteral("File1.txt"));   // natural, any case
        QCOMPARE(name(model, 2), QStringLiteral("file9.txt"));
        QCOMPARE(name(model, 3), QStringLiteral("file10.txt"));
        QCOMPARE(nine.row(), 2);   // the selection follows its row

        model.sort(ObjectTableModel::SizeColumn, Qt::DescendingOrder);
        QCOMPARE(name(model, 0), QStringLiteral("dir"));
        QCOMPARE(name(model, 1), QStringLiteral("File1.txt"));
        QCOMPARE(name(model, 3), QStringLiteral("file10.txt"));

        model.sort(-1);
        QCOMPARE(name(model, 2), QStringLiteral("file10.txt"));
        QCOMPARE(nine.row(), 3);
    }

    void naturalOrderNeedsNoIcu()
    {
        // Digit runs by value, text without case — the same with any
        // QCollator backend (CI's Qt has no ICU).
        for (const char* k : {"b", "a10b10", "A3", "a02x", "a10b2", "a2", "img001.png", "img1000.png", "img20.png"})
            m_bucket.add(QString::fromLatin1(k));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        model.sort(ObjectTableModel::NameColumn, Qt::AscendingOrder);
        QStringList names;
        for (int r = 0; r < model.rowCount(); ++r)
            names.append(name(model, r));
        QCOMPARE(names, (QStringList{QStringLiteral("a2"), QStringLiteral("a02x"), QStringLiteral("A3"),
                                     QStringLiteral("a10b2"), QStringLiteral("a10b10"), QStringLiteral("b"),
                                     QStringLiteral("img001.png"), QStringLiteral("img20.png"),
                                     QStringLiteral("img1000.png")}));
    }

    void largeSortsRunInTheBackgroundAndPagesAreSortedIn()
    {
        for (int i = 0; i < 7000; ++i)
            m_bucket.add(key(i), (i * 7919) % 10007);
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        for (int rows = 1000; rows <= 6000; rows += 1000) {
            QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), rows, 10000);
            if (rows < 6000)
                model.loadMore();
        }
        const QPersistentModelIndex tracked = model.index(4321, 0);
        const QString trackedKey = tracked.data(ObjectTableModel::KeyRole).toString();

        QSignalSpy sorted(&model, &ObjectTableModel::sortFinished);
        model.sort(ObjectTableModel::SizeColumn, Qt::AscendingOrder);
        QVERIFY(model.isSorting());
        QVERIFY(sorted.wait(10000));
        QVERIFY(!model.isSorting());
        QCOMPARE(tracked.data(ObjectTableModel::KeyRole).toString(), trackedKey);
        for (int r = 1; r < model.rowCount(); ++r) {
            QVERIFY(model.index(r - 1, 0).data(ObjectTableModel::SizeRole).toLongLong()
                    <= model.index(r, 0).data(ObjectTableModel::SizeRole).toLongLong());
        }

        // The last page arrives while sorted: appended first, then sorted in.
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 7000);
        QTRY_VERIFY_WITH_TIMEOUT(!model.isSorting(), 10000);
        for (int r = 1; r < model.rowCount(); ++r) {
            QVERIFY(model.index(r - 1, 0).data(ObjectTableModel::SizeRole).toLongLong()
                    <= model.index(r, 0).data(ObjectTableModel::SizeRole).toLongLong());
        }
        QCOMPARE(tracked.data(ObjectTableModel::KeyRole).toString(), trackedKey);
    }

    void liveFilterOnLoadedRows()
    {
        m_bucket.add(QStringLiteral("Reports/x"));
        m_bucket.add(QStringLiteral("archive/x"));
        for (int i = 0; i < 1200; ++i)
            m_bucket.add(key(i));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1000, 10000);

        model.setFilterText(QStringLiteral("R"));   // any case: "Reports", "archive"
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.filterText(), QStringLiteral("R"));
        model.setFilterText(QStringLiteral("f0119"));
        QCOMPARE(model.rowCount(), 0);   // f01190–f01199 are on the next page
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 10);
        model.setFilterText(QString());
        QCOMPARE(model.rowCount(), 1202);
        QCOMPARE(model.loadedObjects(), 1200);   // the counts ignore the filter
    }

    void serverSidePrefixSearch()
    {
        m_bucket.add(QStringLiteral("docs/alpha.txt"));
        m_bucket.add(QStringLiteral("docs/beta.txt"));
        m_bucket.add(QStringLiteral("docs/alpine/x"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("docs/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 3);
        model.setSearchPrefix(QStringLiteral("alp"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(name(model, 0), QStringLiteral("alpine"));
        QCOMPARE(name(model, 1), QStringLiteral("alpha.txt"));
        QCOMPARE(QUrl::fromPercentEncoding(m_server->requests().last().queryItem("prefix")),
                 QStringLiteral("docs/alp"));
        QCOMPARE(model.prefix(), QStringLiteral("docs/"));
    }

    void aReplacedSessionListsAgain()
    {
        m_bucket.add(QStringLiteral("x"));
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        m_sessions->reconnect(m_account);
        QCOMPARE(model.state(), State::Loading);
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(m_server->requestCount(), 2);
    }

    void aRevisitShowsTheCacheStaleThenFresh()
    {
        m_bucket.add(QStringLiteral("a/1"));
        m_bucket.add(QStringLiteral("b/1"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        QSignalSpy stale(&model, &ObjectTableModel::staleChanged);
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("a/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("b/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);

        m_bucket.add(QStringLiteral("a/2"));   // changed on the server meanwhile
        m_bucket.sort();
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("a/"));
        // At once, before any answer: the cached rows, marked stale.
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(model.isStale());
        QVERIFY(model.staleSince().isValid());
        QVERIFY(!model.canLoadMore());
        QCOMPARE(stale.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!model.isStale(), 10000);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.state(), State::Complete);
        QCOMPARE(stale.size(), 2);
    }

    void aStaleRefreshRefetchesTheCachedPages()
    {
        for (int i = 0; i < 2500; ++i)
            m_bucket.add(QStringLiteral("big/") + key(i));
        m_bucket.add(QStringLiteral("small/x"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("big/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1000, 10000);
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2000, 10000);
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("small/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        m_server->clearRequests();

        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("big/"));
        QCOMPARE(model.rowCount(), 2000);
        QVERIFY(model.isStale());
        QTRY_VERIFY_WITH_TIMEOUT(!model.isStale(), 10000);
        QCOMPARE(m_server->requestCount(), 2);   // both cached pages, then swapped
        QCOMPARE(model.rowCount(), 2000);
        QCOMPARE(model.state(), State::Ready);
        model.loadMore();
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 2500);
    }

    void reloadNeverUsesTheCache()
    {
        m_bucket.add(QStringLiteral("x"));
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        model.reload();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.isStale());
        QCOMPARE(model.state(), State::Loading);
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
    }

    void aFailedRefreshKeepsTheStaleRows()
    {
        m_bucket.add(QStringLiteral("a/1"));
        m_bucket.add(QStringLiteral("b/1"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("a/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("b/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        m_server->setFallbackHandler([](const RecordedRequest&) { return FakeResponse::s3Error(403, "AccessDenied"); });
        model.setLocation(m_account, QStringLiteral("bkt"), QStringLiteral("a/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Error, 10000);
        QVERIFY(model.isStale());   // never passed off as current
        QCOMPARE(model.rowCount(), 1);
    }

    void searchesAreNotCached()
    {
        m_bucket.add(QStringLiteral("alpha"));
        m_bucket.add(QStringLiteral("beta"));
        m_bucket.sort();
        ObjectTableModel model(m_sessions.get());
        model.setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        model.setSearchPrefix(QStringLiteral("al"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), State::Complete, 10000);
        QCOMPARE(model.rowCount(), 1);
        model.setSearchPrefix(QString());   // back: the unsearched listing, from the cache
        QVERIFY(model.isStale());
        QCOMPARE(model.rowCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!model.isStale(), 10000);
    }

private:
    QTemporaryDir m_dir;
    int m_run = 0;
    FakeBucket m_bucket;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
    QUuid m_account;
};

QTEST_GUILESS_MAIN(TestObjectTableModel)
#include "test_object_table_model.moc"
