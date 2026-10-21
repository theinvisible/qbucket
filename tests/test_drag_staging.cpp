// Dragging out the Linux / Flatpak way: DragStaging prepares local copies
// with transient DownloadJobs (quietly only for small selections of
// objects, asking above the limit), knows when they are ready, expires and
// cleans them up; StagingMimeData hands the files to a drop target only when
// they are there.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/DragStaging.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "platform/StagingMimeData.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QUrl>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {
const QString kBucket = QStringLiteral("bkt");
} // namespace

class TestDragStaging : public QObject {
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
    std::function<void(Job*)> m_onPreview;

    DragStaging::Selection selection(const QList<DragStaging::Item>& items) const
    {
        return {m_h->account.id, kBucket, items};
    }

    static QByteArray read(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        Account a = m_h->account;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_objects->putObject(kBucket, QStringLiteral("docs/a.txt"), "aaa");
        m_objects->putObject(kBucket, QStringLiteral("docs/b.txt"), "bbbb");
        m_objects->putObject(kBucket, QStringLiteral("photos/x.jpg"), "xx");
        m_objects->putObject(kBucket, QStringLiteral("photos/2026/y.jpg"), "yyy");
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
        m_onPreview = [](Job* job) { job->confirm(); };
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) { m_onPreview(job); });
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

    void smallSelectionsAreStagedQuietly()
    {
        DragStaging staging(m_ops.get(), m_h->path(QStringLiteral("drag")));
        staging.setConfirmAboveBytes(100);
        const auto files = selection({{QStringLiteral("docs/a.txt"), false, 3}, {QStringLiteral("docs/b.txt"), false, 4}});
        QVERIFY(staging.stagesQuietly(files));
        QVERIFY(!staging.stagesQuietly(selection({{QStringLiteral("photos/"), true, -1}})));   // size unknown
        QVERIFY(!staging.stagesQuietly(selection({{QStringLiteral("docs/a.txt"), false, 1000}})));   // above the limit
        QVERIFY(!staging.stagesQuietly(selection({{QStringLiteral("docs/a.txt"), false, -1}})));

        QVERIFY(staging.readyPaths(files).isEmpty());
        staging.prepare(files, true);
        QCOMPARE(staging.state(files), DragStaging::State::Preparing);
        QTRY_COMPARE_WITH_TIMEOUT(staging.state(files), DragStaging::State::Ready, 10000);
        const QStringList paths = staging.readyPaths(files);
        QCOMPARE(paths.size(), 2);
        QVERIFY(paths.first().startsWith(staging.root()));
        QCOMPARE(QFileInfo(paths.at(0)).fileName(), QStringLiteral("a.txt"));
        QCOMPARE(read(paths.at(1)), QByteArray("bbbb"));
        // The same selection in another order is the same staging.
        QCOMPARE(staging.readyPaths(selection({{QStringLiteral("docs/b.txt"), false, 4}, {QStringLiteral("docs/a.txt"), false, 3}})),
                 paths);
        // A drop target that moved the copy away: not ready any more.
        QVERIFY(QFile::remove(paths.at(0)));
        QCOMPARE(staging.state(files), DragStaging::State::None);
        QVERIFY(staging.readyPaths(files).isEmpty());
        // A quiet prepare of a folder does nothing.
        const auto folder = selection({{QStringLiteral("photos/"), true, -1}});
        staging.prepare(folder, true);
        QCOMPARE(staging.state(folder), DragStaging::State::None);
    }

    void foldersAndLargeOnesOnRequestAskingAboveTheLimit()
    {
        DragStaging staging(m_ops.get(), m_h->path(QStringLiteral("drag")));
        staging.setConfirmAboveBytes(4);
        int previews = 0;
        m_onPreview = [&](Job* job) {
            ++previews;
            job->confirm();
        };
        const auto folder = selection({{QStringLiteral("photos/"), true, -1}});
        staging.prepare(folder, false);
        QTRY_COMPARE_WITH_TIMEOUT(staging.state(folder), DragStaging::State::Ready, 10000);
        QCOMPARE(previews, 1);   // 5 bytes > 4: asked
        const QStringList paths = staging.readyPaths(folder);
        QCOMPARE(paths.size(), 1);
        QVERIFY(QFileInfo(paths.first()).isDir());
        QCOMPARE(read(paths.first() + QStringLiteral("/2026/y.jpg")), QByteArray("yyy"));

        // Declined: nothing staged, nothing left behind.
        m_onPreview = [](Job* job) { job->cancel(); };
        const auto docs = selection({{QStringLiteral("docs/"), true, -1}});
        staging.prepare(docs, false);
        QTRY_COMPARE_WITH_TIMEOUT(staging.state(docs), DragStaging::State::Failed, 10000);
        QVERIFY(staging.readyPaths(docs).isEmpty());
        QCOMPARE(QDir(staging.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);   // photos only
    }

    void expiredCopiesAndStrayFoldersAreRemoved()
    {
        const QString root = m_h->path(QStringLiteral("drag"));
        QVERIFY(QDir().mkpath(root + u'/' + QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/not-ours")));
        DragStaging staging(m_ops.get(), root);   // a new session sweeps
        QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{QStringLiteral("not-ours")});
        const auto files = selection({{QStringLiteral("docs/a.txt"), false, 3}});
        staging.prepare(files, true);
        QTRY_COMPARE_WITH_TIMEOUT(staging.state(files), DragStaging::State::Ready, 10000);
        staging.setMaxAgeSecs(-1);
        QVERIFY(staging.readyPaths(files).isEmpty());
        staging.cleanup();
        QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{QStringLiteral("not-ours")});
    }

    void mimeDataHandsOverFilesOnlyWhenReady()
    {
        DragStaging staging(m_ops.get(), m_h->path(QStringLiteral("drag")));
        staging.setConfirmAboveBytes(100);
        const auto files = selection({{QStringLiteral("docs/a.txt"), false, 3}});
        auto internal = std::make_unique<QMimeData>();
        internal->setData(QStringLiteral("application/x-qbucket-objects"), "{}");
        internal->setText(QStringLiteral("s3://bkt/docs/a.txt"));
        StagingMimeData mime(std::move(internal), &staging, files);
        QVERIFY(mime.formats().contains(QStringLiteral("text/uri-list")));
        QVERIFY(mime.hasFormat(QStringLiteral("application/x-qbucket-objects")));
        QCOMPARE(mime.data(QStringLiteral("application/x-qbucket-objects")), QByteArray("{}"));

        QSignalSpy message(&staging, &DragStaging::message);
        QVERIFY(mime.urls().isEmpty());   // too early: nothing, and a note
        QCOMPARE(message.size(), 1);
        (void)mime.urls();
        QCOMPARE(message.size(), 1);      // once per drag

        staging.prepare(files, true);
        QTRY_COMPARE_WITH_TIMEOUT(staging.state(files), DragStaging::State::Ready, 10000);
        const QList<QUrl> urls = mime.urls();
        QCOMPARE(urls.size(), 1);
        QVERIFY(urls.first().isLocalFile());
        QCOMPARE(read(urls.first().toLocalFile()), QByteArray("aaa"));
        QVERIFY(mime.data(QStringLiteral("text/uri-list")).startsWith("file://"));
    }
};

QTEST_GUILESS_MAIN(TestDragStaging)
#include "test_drag_staging.moc"
