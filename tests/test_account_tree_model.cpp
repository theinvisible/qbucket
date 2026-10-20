#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/AccountTreeModel.h"
#include "support/FakeKeychainBackend.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;

namespace {

const QString kKeyId = QStringLiteral("AKIDTREE");
const QString kSecret = QStringLiteral("tree-secret");

QStringList L(std::initializer_list<const char*> names)
{
    QStringList out;
    for (const char* n : names)
        out.append(QString::fromLatin1(n));
    return out;
}

QByteArray bucketList(std::initializer_list<const char*> names)
{
    QByteArray xml = "<ListAllMyBucketsResult><Buckets>";
    for (const char* n : names)
        xml += QByteArray("<Bucket><Name>") + n + "</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>";
    return xml + "</Buckets></ListAllMyBucketsResult>";
}

using Kind = AccountTreeModel::Kind;
using Status = AccountTreeModel::Status;

Kind kindOf(const QModelIndex& i)
{
    return Kind(i.data(AccountTreeModel::KindRole).toInt());
}

Status statusOf(const QModelIndex& i)
{
    return Status(i.data(AccountTreeModel::StatusRole).toInt());
}

QStringList childNames(const AccountTreeModel& m, const QModelIndex& parent)
{
    QStringList out;
    for (int r = 0; r < m.rowCount(parent); ++r) {
        const QModelIndex c = m.index(r, 0, parent);
        if (kindOf(c) == Kind::Bucket)
            out.append(c.data().toString());
    }
    return out;
}

} // namespace

class TestAccountTreeModel : public QObject {
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

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts-%1.ini").arg(++m_run)));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        m_keychain = backend.get();
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        m_sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
    }

    void cleanup()
    {
        m_sessions.reset();
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void bucketsLoadLazilyAndSorted()
    {
        FakeResponse slow = FakeResponse::ok(bucketList({"zeta", "Alpha", "beta"}));
        slow.delayMs = 300;
        m_server->on("GET", QStringLiteral("^/$"), slow);
        const Account a = addAccount(QStringLiteral("one"), {QStringLiteral("shared")});
        AccountTreeModel model(m_accounts.get(), m_sessions.get());

        QCOMPARE(model.rowCount(), 1);
        const QModelIndex acc = model.accountIndex(a.id);
        QCOMPARE(kindOf(acc), Kind::Account);
        QCOMPARE(acc.data().toString(), QStringLiteral("one"));
        QVERIFY(model.hasChildren(acc));
        QCOMPARE(model.rowCount(acc), 0);
        QVERIFY(model.canFetchMore(acc));
        QTest::qWait(50);
        QCOMPARE(m_server->requestCount(), 0);   // nothing until expanded

        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.fetchMore(acc);
        QVERIFY(!model.canFetchMore(acc));
        QCOMPARE(model.rowCount(acc), 0);   // rows change from the event loop only
        // While loading: the status row, and the external bucket already.
        QTRY_COMPARE(model.rowCount(acc), 2);
        QCOMPARE(statusOf(model.index(0, 0, acc)), Status::Loading);

        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, acc), L({"Alpha", "beta", "shared", "zeta"}), 10000);
        QCOMPARE(model.rowCount(acc), 4);   // no status row any more
        QCOMPARE(m_server->requestCount(), 1);
        const QModelIndex shared = model.bucketIndex(a.id, QStringLiteral("shared"));
        QVERIFY(shared.isValid());
        QVERIFY(shared.data(AccountTreeModel::ExternalRole).toBool());
        QVERIFY(!model.bucketIndex(a.id, QStringLiteral("beta")).data(AccountTreeModel::ExternalRole).toBool());
        QCOMPARE(shared.data(AccountTreeModel::AccountIdRole).toUuid(), a.id);
        QCOMPARE(shared.parent(), acc);
    }

    void listedExternalBucketAppearsOnce()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({"shared"})));
        const Account a = addAccount(QStringLiteral("one"), {QStringLiteral("shared")});
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        model.fetchMore(model.accountIndex(a.id));
        // Listed: shown once, and not as an external bucket.
        const auto listedRow = [&] {
            const QModelIndex i = model.bucketIndex(a.id, QStringLiteral("shared"));
            return i.isValid() && !i.data(AccountTreeModel::ExternalRole).toBool();
        };
        QTRY_VERIFY_WITH_TIMEOUT(listedRow(), 10000);
        QCOMPARE(childNames(model, model.accountIndex(a.id)), L({"shared"}));
    }

    void errorIsAStatusRowNeverAnEmptyList()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::s3Error(403, "SignatureDoesNotMatch"));
        const Account a = addAccount(QStringLiteral("one"));
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        const QModelIndex acc = model.accountIndex(a.id);
        model.fetchMore(acc);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model.index(0, 0, acc)), Status::Error, 10000);
        QCOMPARE(model.rowCount(acc), 1);
        QVERIFY(!model.index(0, 0, acc).data().toString().isEmpty());
        QVERIFY(!model.canFetchMore(acc));
    }

    void emptyListShowsEmptyRow()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({})));
        const Account a = addAccount(QStringLiteral("one"));
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        const QModelIndex acc = model.accountIndex(a.id);
        model.fetchMore(acc);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model.index(0, 0, acc)), Status::Empty, 10000);
        QCOMPARE(model.rowCount(acc), 1);
    }

    void forbiddenListingStillShowsExternalBuckets()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::s3Error(403, "AccessDenied"));
        const Account a = addAccount(QStringLiteral("one"), {QStringLiteral("shared")});
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        const QModelIndex acc = model.accountIndex(a.id);
        model.fetchMore(acc);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model.index(0, 0, acc)), Status::ListingNotPermitted, 10000);
        QCOMPARE(childNames(model, acc), L({"shared"}));
    }

    void accountsAddedAndRemovedKeepOtherListings()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({"b1"})));
        const Account one = addAccount(QStringLiteral("one"));
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.fetchMore(model.accountIndex(one.id));
        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, model.accountIndex(one.id)), L({"b1"}), 10000);
        const qsizetype requests = m_server->requestCount();

        const Account zero = addAccount(QStringLiteral("aaa first"));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("aaa first"));
        QCOMPARE(childNames(model, model.accountIndex(one.id)), L({"b1"}));   // kept
        // (The model tester expands the new account itself.)

        QVERIFY(m_accounts->remove(zero.id));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(childNames(model, model.accountIndex(one.id)), L({"b1"}));
        QCOMPARE(m_server->requestCount(), requests);
    }

    void connectionChangeListsAgain()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({"b1"})));
        Account a = addAccount(QStringLiteral("one"));
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.fetchMore(model.accountIndex(a.id));
        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, model.accountIndex(a.id)), L({"b1"}), 10000);

        m_server->clearRoutes();
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({"b2"})));
        a.addressing = s3::AddressingStyle::Path;   // a connection setting
        QVERIFY(m_accounts->save(a));
        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, model.accountIndex(a.id)), L({"b2"}), 10000);
    }

    void externalBucketEditsShowAtOnce()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({"b1"})));
        Account a = addAccount(QStringLiteral("one"));
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.fetchMore(model.accountIndex(a.id));
        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, model.accountIndex(a.id)), L({"b1"}), 10000);
        const qsizetype requests = m_server->requestCount();

        a.externalBuckets = {QStringLiteral("added")};
        QVERIFY(m_accounts->save(a));
        QCOMPARE(childNames(model, model.accountIndex(a.id)), L({"added", "b1"}));
        QCOMPARE(m_server->requestCount(), requests);   // no new listing needed
    }

    void refreshDropsTheOlderAnswer()
    {
        FakeResponse slow = FakeResponse::ok(bucketList({"old"}));
        slow.delayMs = 800;
        Matcher root;
        root.method = "GET";
        root.path = QRegularExpression(QStringLiteral("^/$"));
        m_server->addRoute(root, {slow, FakeResponse::ok(bucketList({"new"}))});
        const Account a = addAccount(QStringLiteral("one"));
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        const QModelIndex acc = model.accountIndex(a.id);
        model.fetchMore(acc);
        QTRY_COMPARE(m_server->requestCount(), 1);
        model.refresh(acc);
        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, acc), L({"new"}), 10000);
        QTest::qWait(1200);   // the first, cancelled listing must not come back
        QCOMPARE(childNames(model, acc), L({"new"}));
    }

    void secretMissingReportsCredentials()
    {
        m_server->on("GET", QStringLiteral("^/$"), FakeResponse::ok(bucketList({"b1"})));
        const Account a = addAccount(QStringLiteral("one"), {}, false);
        AccountTreeModel model(m_accounts.get(), m_sessions.get());
        QSignalSpy needed(m_secrets.get(), &SecretStore::secretNeeded);
        const QModelIndex acc = model.accountIndex(a.id);
        model.fetchMore(acc);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model.index(0, 0, acc)), Status::Error, 10000);
        QCOMPARE(needed.size(), 1);

        // What the sidebar does with the user's answer.
        m_secrets->setSessionSecret(a.id, SecretStore::Kind::SecretAccessKey, kSecret);
        m_sessions->reconnect(a.id);
        model.refresh(acc);
        QTRY_COMPARE_WITH_TIMEOUT(childNames(model, acc), L({"b1"}), 10000);
    }

private:
    Account addAccount(const QString& name, const QStringList& external = {}, bool withSecret = true)
    {
        Account a;
        a.id = QUuid::createUuid();
        a.name = name;
        a.providerId = QStringLiteral("generic");
        a.region = QStringLiteral("test-1");
        a.endpoint = m_server->endpointUrl().toString();
        a.accessKeyId = kKeyId;
        a.externalBuckets = external;
        if (withSecret)
            m_keychain->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                           + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                       kSecret);
        m_accounts->save(a);
        return a;
    }

    QTemporaryDir m_dir;
    int m_run = 0;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    testing::FakeKeychainBackend* m_keychain = nullptr;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
};

QTEST_GUILESS_MAIN(TestAccountTreeModel)
#include "test_account_tree_model.moc"
