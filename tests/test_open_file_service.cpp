// OpenFileService against FakeObjectStore: an object downloaded into the
// cache and handed to "the default application" (recorded), a change saved
// the way editors save (another file renamed over it) offered for upload, a
// touch without a change ignored, the upload through ReplaceObjectJob,
// versions and read-only accounts write-protected, changed files kept for
// the next session and everything else cleaned up.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OpenFileService.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {
const QString kBucket = QStringLiteral("bkt");
const QString kKey = QStringLiteral("notes/todo.txt");
} // namespace

class TestOpenFileService : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<TransferService> m_transfers;
    std::unique_ptr<OperationController> m_ops;
    QList<QUrl> m_opened;

    std::unique_ptr<OpenFileService> makeService()
    {
        auto s = std::make_unique<OpenFileService>(m_ops.get(), m_h->path(QStringLiteral("open")),
                                                   m_h->path(QStringLiteral("open-files.json")));
        s->setOpener([this](const QUrl& url) {
            m_opened.append(url);
            return true;
        });
        s->setSettleDelay(50);
        return s;
    }

    // As editors save: a new file renamed over the old one.
    static bool saveAsEditor(const QString& path, const QByteArray& content)
    {
        const QString temp = path + QStringLiteral(".tmp~");
        QFile f(temp);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(content);
        f.close();
        QFile::remove(path);
        return QFile::rename(temp, path);
    }

    static const OpenFileService::Entry* only(const OpenFileService& s)
    {
        return s.entries().size() == 1 ? &s.entries().first() : nullptr;
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        Account a = m_h->account;
        a.providerId = QStringLiteral("aws");
        a.addressing = s3::AddressingStyle::Path;
        a.environment = Environment::Development;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_objects->setVersioning(kBucket, QStringLiteral("Enabled"));
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_transfers = std::make_unique<TransferService>(m_h->path(QStringLiteral("transfers")), nullptr);
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        m_ops->setTransfers(m_transfers.get());
        connect(m_ops.get(), &OperationController::previewNeeded, this, [](Job* job) { job->confirm(); });
        m_objects->putObject(kBucket, kKey, "buy milk\n", QStringLiteral("text/plain"));
        m_opened.clear();
    }

    void cleanup()
    {
        m_ops.reset();
        m_transfers.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void opensEditsAndUploads()
    {
        auto service = makeService();
        service->open(m_h->account.id, kBucket, kKey);
        QTRY_VERIFY_WITH_TIMEOUT(only(*service) && only(*service)->state == OpenFileService::State::Open, 10000);
        const OpenFileService::Entry e = *only(*service);
        QCOMPARE(QFileInfo(e.path).fileName(), QStringLiteral("todo.txt"));
        QVERIFY(e.path.startsWith(service->root()));
        QCOMPARE(m_opened, QList<QUrl>{QUrl::fromLocalFile(e.path)});
        QVERIFY(!e.readOnly);
        // A second open of the same object opens the same copy.
        service->open(m_h->account.id, kBucket, kKey);
        QCOMPARE(service->entries().size(), 1);
        QCOMPARE(m_opened.size(), 2);

        // A touch alone is no change.
        QSignalSpy changed(service.get(), &OpenFileService::fileChanged);
        {
            QFile f(e.path);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(5), QFileDevice::FileModificationTime));
        }
        QTest::qWait(400);
        QCOMPARE(changed.size(), 0);

        // Saved the way editors save.
        QVERIFY(saveAsEditor(e.path, "buy milk\nand bread\n"));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000);
        QCOMPARE(only(*service)->state, OpenFileService::State::Changed);
        QCOMPARE(service->changedEntries().size(), 1);

        service->upload(e.id);
        QTRY_COMPARE_WITH_TIMEOUT(only(*service)->state, OpenFileService::State::Open, 15000);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("buy milk\nand bread\n"));
        QCOMPARE(m_objects->object(kBucket, kKey)->contentType, QStringLiteral("text/plain"));
        QCOMPARE(only(*service)->etag, m_objects->object(kBucket, kKey)->etag);

        // Edited again: offered again, and "ignore" takes it as it is.
        QVERIFY(saveAsEditor(e.path, "buy milk\nand bread\nand eggs\n"));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 2, 5000);
        service->ignoreChange(e.id);
        QCOMPARE(only(*service)->state, OpenFileService::State::Open);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("buy milk\nand bread\n"));
    }

    void versionsAndReadOnlyAccountsAreWriteProtected()
    {
        const QString old = m_objects->versions(kBucket, kKey).first().versionId;
        QVERIFY(!old.isEmpty());
        m_objects->putObject(kBucket, kKey, "newer\n", QStringLiteral("text/plain"));
        auto service = makeService();
        service->open(m_h->account.id, kBucket, kKey, old);
        QTRY_VERIFY_WITH_TIMEOUT(only(*service) && only(*service)->state == OpenFileService::State::Open, 10000);
        const OpenFileService::Entry e = *only(*service);
        QVERIFY(e.readOnly);
        QVERIFY(!(QFile::permissions(e.path) & QFileDevice::WriteOwner));
        QFile f(e.path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("buy milk\n"));
        service->upload(e.id);   // never
        QCOMPARE(only(*service)->state, OpenFileService::State::Open);
        service->discard(e.id);
        QVERIFY(service->entries().isEmpty());
        QVERIFY(!QFileInfo::exists(e.path));
    }

    void changedFilesSurviveQuittingTheRestIsCleanedUp()
    {
        QString keptPath;
        QString cleanPath;
        {
            auto service = makeService();
            m_objects->putObject(kBucket, QStringLiteral("other.txt"), "x", QStringLiteral("text/plain"));
            service->open(m_h->account.id, kBucket, kKey);
            service->open(m_h->account.id, kBucket, QStringLiteral("other.txt"));
            QTRY_VERIFY_WITH_TIMEOUT(service->entries().size() == 2
                                         && service->entries().at(0).state == OpenFileService::State::Open
                                         && service->entries().at(1).state == OpenFileService::State::Open,
                                     10000);
            const OpenFileService::Entry todo = service->entries().at(0).key == kKey ? service->entries().at(0)
                                                                                     : service->entries().at(1);
            QSignalSpy changed(service.get(), &OpenFileService::fileChanged);
            QVERIFY(saveAsEditor(todo.path, "changed\n"));
            QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000);
            keptPath = todo.path;
            for (const auto& e : service->entries()) {
                if (e.key != kKey)
                    cleanPath = e.path;
            }
            service->keepForLater();
            QCOMPARE(service->entries().size(), 1);
        }
        QVERIFY(QFileInfo::exists(keptPath));
        QVERIFY(!QFileInfo::exists(cleanPath));
        QVERIFY(QFileInfo::exists(m_h->path(QStringLiteral("open-files.json"))));
        // A stray folder of an older session goes; a non-uuid folder stays.
        QDir root(m_h->path(QStringLiteral("open")));
        QVERIFY(root.mkpath(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(root.mkpath(QStringLiteral("not-ours")));

        auto again = makeService();
        QCOMPARE(again->entries().size(), 1);
        QCOMPARE(again->entries().first().state, OpenFileService::State::Changed);
        QCOMPARE(again->entries().first().path, keptPath);
        again->cleanup();
        QCOMPARE(root.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);   // the kept one, not-ours
        again->upload(again->entries().first().id);
        QTRY_COMPARE_WITH_TIMEOUT(again->entries().first().state, OpenFileService::State::Open, 15000);
        QCOMPARE(m_objects->object(kBucket, kKey)->data, QByteArray("changed\n"));
        QVERIFY(!QFileInfo::exists(m_h->path(QStringLiteral("open-files.json"))));   // nothing left to keep
    }

    void aFailedDownloadLeavesNothing()
    {
        auto service = makeService();
        QSignalSpy message(service.get(), &OpenFileService::message);
        service->open(m_h->account.id, kBucket, QStringLiteral("missing.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(service->entries().isEmpty() && message.size() == 1, 10000);
        QVERIFY(m_opened.isEmpty());
        QCOMPARE(QDir(m_h->path(QStringLiteral("open"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestOpenFileService)
#include "test_open_file_service.moc"
