#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/ObjectTableModel.h"
#include "support/FakeBucket.h"
#include "support/FakeKeychainBackend.h"
#include "ui/DetailPane.h"
#include "ui/ObjectRowDelegate.h"
#include "ui/ThemeManager.h"

#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::FakeBucket;

namespace {

const QString kKeyId = QStringLiteral("AKIDDETAILS");
const QString kSecret = QStringLiteral("details-secret");

QStringList fieldTexts(const DetailPane& pane)
{
    QStringList out;
    for (const QLabel* l : pane.findChildren<QLabel*>()) {
        if (!l->isHidden() && l->objectName().isEmpty())
            out.append(l->text());
    }
    return out;
}

} // namespace

class TestDetailPane : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_theme.apply(QStringLiteral("Light"));
    }

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
        m_bucket.add(QStringLiteral("docs/x"));
        m_bucket.add(QStringLiteral("a.txt"), 1234);
        m_bucket.add(QStringLiteral("b.tar.gz"), 5678, "GLACIER");
        m_bucket.add(QStringLiteral("c.jpg"), 10, "STANDARD_IA");
        m_bucket.sort();
        FakeResponse head;
        head.withHeader("Content-Length", "1234")
            .withHeader("Last-Modified", "Tue, 20 Oct 2026 08:00:00 GMT")
            .withHeader("ETag", "\"abc\"")
            .withHeader("Content-Type", "text/plain")
            .withHeader("Cache-Control", "max-age=60")
            .withHeader("x-amz-meta-owner", "ops");
        m_server->on("HEAD", QStringLiteral("^/bkt/a\\.txt$"), head);
        m_server->on("HEAD", QStringLiteral("^/bkt/b\\.tar\\.gz$"), FakeResponse::s3Error(403, "AccessDenied"));
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
        m_model = std::make_unique<ObjectTableModel>(m_sessions.get());
        m_model->setLocation(m_account, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(m_model->state(), ObjectTableModel::State::Complete, 10000);
        QCOMPARE(m_model->rowCount(), 4);   // docs, a.txt, b.tar.gz, c.jpg
    }

    void cleanup()
    {
        m_model.reset();
        m_sessions.reset();
        m_secrets.reset();
        m_accounts.reset();
        m_server.reset();
    }

    void oneObjectIsAskedForOnceTheSelectionRests()
    {
        DetailPane pane(m_sessions.get());
        QSignalSpy done(&pane, &DetailPane::headFinished);
        m_server->clearRequests();
        // Arrow keys through the list: only where it stops is asked for.
        pane.setSelection(m_account, QStringLiteral("bkt"), {m_model->index(3, 0)});
        pane.setSelection(m_account, QStringLiteral("bkt"), {m_model->index(2, 0)});
        pane.setSelection(m_account, QStringLiteral("bkt"), {m_model->index(1, 0)});
        QVERIFY(fieldTexts(pane).contains(QStringLiteral("a.txt")));   // at once, from the listing
        QVERIFY(done.wait(10000));
        QCOMPARE(m_server->requestCount(), 1);
        QCOMPARE(m_server->requests().first().method, QByteArray("HEAD"));
        QCOMPARE(pane.meta().contentType, QStringLiteral("text/plain"));
        QVERIFY(fieldTexts(pane).contains(QStringLiteral("\"abc\"")));

        const auto* headers = pane.findChild<QTreeWidget*>(QStringLiteral("headerList"));
        QVERIFY(headers);
        QStringList names;
        for (int i = 0; i < headers->topLevelItem(0)->childCount(); ++i)
            names.append(headers->topLevelItem(0)->child(i)->text(0).toLower());
        QVERIFY(names.contains(QStringLiteral("cache-control")));
        QVERIFY(!names.contains(QStringLiteral("x-amz-meta-owner")));   // listed as user metadata
        QCOMPARE(headers->topLevelItem(1)->child(0)->text(0), QStringLiteral("owner"));
        QCOMPARE(headers->topLevelItem(1)->child(0)->text(1), QStringLiteral("ops"));
    }

    void foldersAndSeveralRowsNeedNoRequest()
    {
        DetailPane pane(m_sessions.get());
        m_server->clearRequests();
        pane.setSelection(m_account, QStringLiteral("bkt"), {m_model->index(0, 0)});
        QVERIFY(fieldTexts(pane).contains(QStringLiteral("docs")));
        pane.setSelection(m_account, QStringLiteral("bkt"),
                          {m_model->index(1, 0), m_model->index(1, 2), m_model->index(3, 0), m_model->index(0, 0)});
        QVERIFY(fieldTexts(pane).join(u' ').contains(QLatin1String("3")));   // three rows, a.txt counted once
        QTest::qWait(DetailPane::kHeadDelayMs * 3);
        QCOMPARE(m_server->requestCount(), 0);
        pane.clear();
        QTest::qWait(DetailPane::kHeadDelayMs * 2);
        QCOMPARE(m_server->requestCount(), 0);
    }

    void anErrorIsShown()
    {
        DetailPane pane(m_sessions.get());
        QSignalSpy done(&pane, &DetailPane::headFinished);
        pane.setSelection(m_account, QStringLiteral("bkt"), {m_model->index(2, 0)});
        QVERIFY(done.wait(10000));
        QVERIFY(fieldTexts(pane).join(u'\n').contains(QLatin1String("may not")));
    }

    void rowsPaintFromRolesOnly()
    {
        // Smoke test for the delegate in both themes: no I/O, no crash.
        QTreeView view;
        view.setItemDelegate(new ObjectRowDelegate(&view));
        view.setModel(m_model.get());
        view.resize(600, 200);
        view.show();
        m_server->clearRequests();
        QVERIFY(!view.grab().isNull());
        m_theme.apply(QStringLiteral("Dark"));
        QVERIFY(!view.grab().isNull());
        QCOMPARE(m_server->requestCount(), 0);
        m_theme.apply(QStringLiteral("Light"));
    }

private:
    ThemeManager m_theme;
    QTemporaryDir m_dir;
    int m_run = 0;
    FakeBucket m_bucket;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
    std::unique_ptr<ObjectTableModel> m_model;
    QUuid m_account;
};

QTEST_MAIN(TestDetailPane)
#include "test_detail_pane.moc"
