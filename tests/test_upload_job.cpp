// UploadJob through the OperationController against FakeObjectStore: plan
// (scan + target lookup), preview with conflicts, the policies, pause,
// cancel, retry, the gate.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferJobStore.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"
#include "support/AccountHarness.h"
#include "ui/OperationPreviewDialog.h"

#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QTranslator>
#include <QtTest>

#include <optional>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");

QByteArray content(int i, int size = 100)
{
    return QByteArray("file ") + QByteArray::number(i) + QByteArray(size, char('a' + i % 26));
}

} // namespace

class TestUploadJob : public QObject {
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
    QString m_local;

    void setEnvironment(Environment env, bool readOnly = false)
    {
        Account a = m_h->account;
        a.environment = env;
        a.readOnly = readOnly;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    QString writeLocal(const QString& rel, const QByteArray& data)
    {
        QString path = m_local + u'/' + rel;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return {};
        return path;
    }

    UploadJob* make(const QStringList& sources, const QString& prefix = QStringLiteral("backup/"), bool alwaysConfirm = true)
    {
        UploadJob::Options o;
        o.alwaysConfirm = alwaysConfirm;
        return new UploadJob(m_h->account, kBucket, prefix, sources, m_ops->planDir(), o);
    }

    // Starts, waits for the preview, confirms, waits for the end.
    bool runConfirmed(Job* job, Job::ConflictPolicy policy = Job::ConflictPolicy::Skip)
    {
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        if (!preview.wait(20000))
            return false;
        job->setConflictPolicy(policy);
        job->confirm();
        return done.wait(60000) || done.size() == 1;
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
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
        m_local = m_h->path(QStringLiteral("local"));
        QDir().mkpath(m_local);
    }

    void cleanup()
    {
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        m_ops.reset();
        m_transfers.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void aFolderIsUploadedCompletely()
    {
        for (int i = 0; i < 30; ++i)
            writeLocal(QStringLiteral("photos/%1/img%2.jpg").arg(i % 3).arg(i), content(i));
        writeLocal(QStringLiteral("photos/readme.txt"), "hello");
        QDir().mkpath(m_local + QStringLiteral("/photos/empty"));
        auto* job = make({m_local + QStringLiteral("/photos")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        const Job::Preview& p = job->preview();
        QCOMPARE(p.objects, 32);   // 31 files + 1 folder marker
        QCOMPARE(p.conflicts, 0);
        QCOMPARE(p.scope, QStringLiteral("s3://bkt/backup/"));
        QVERIFY(!p.samples.isEmpty());
        QVERIFY(!job->isDestructive());
        QVERIFY(m_objects->keys(kBucket).isEmpty());   // planning wrote nothing
        job->confirm();
        QVERIFY(done.wait(60000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 32);
        QCOMPARE(job->md5Verified(), 31);
        QCOMPARE(m_objects->keys(kBucket).size(), 32);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/photos/2/img5.jpg"))->data, content(5));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/photos/readme.txt"))->contentType,
                 QStringLiteral("text/plain"));
        QVERIFY(m_objects->object(kBucket, QStringLiteral("backup/photos/empty/")));
        QCOMPARE(job->bytesDone(), job->bytesTotal());
        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll().count("\"result\":\"ok\""), 32);
    }

    void existingTargetsAreSkippedByDefault()
    {
        writeLocal(QStringLiteral("docs/a.txt"), "new a");
        writeLocal(QStringLiteral("docs/b.txt"), "new b");
        writeLocal(QStringLiteral("single.txt"), "new single");
        m_objects->putObject(kBucket, QStringLiteral("backup/docs/a.txt"), "old a");
        m_objects->putObject(kBucket, QStringLiteral("backup/single.txt"), "old single");
        auto* job = make({m_local + QStringLiteral("/docs"), m_local + QStringLiteral("/single.txt")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        QCOMPARE(job->preview().conflicts, 2);
        QCOMPARE(job->preview().conflictSamples.size(), 2);
        QVERIFY(job->preview().conflictSamples.join(u'\n').contains(QLatin1String("backup/docs/a.txt")));
        job->confirm();   // policy: skip
        QVERIFY(done.wait(30000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 1);
        QCOMPARE(job->report().skipped, 2);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/docs/a.txt"))->data, QByteArray("old a"));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/single.txt"))->data, QByteArray("old single"));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/docs/b.txt"))->data, QByteArray("new b"));
    }

    void overwriteIsDestructiveAndReplaces()
    {
        writeLocal(QStringLiteral("a.txt"), "new");
        m_objects->putObject(kBucket, QStringLiteral("backup/a.txt"), "old");
        auto* job = make({m_local + QStringLiteral("/a.txt")});
        QVERIFY(runConfirmed(job, Job::ConflictPolicy::Overwrite));
        QVERIFY(job->isDestructive());
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/a.txt"))->data, QByteArray("new"));
    }

    void keepBothUploadsUnderAFreeName()
    {
        writeLocal(QStringLiteral("report.tar.gz"), "new");
        m_objects->putObject(kBucket, QStringLiteral("backup/report.tar.gz"), "old");
        m_objects->putObject(kBucket, QStringLiteral("backup/report (1).tar.gz"), "older copy");
        auto* job = make({m_local + QStringLiteral("/report.tar.gz")});
        QVERIFY(runConfirmed(job, Job::ConflictPolicy::KeepBoth));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/report.tar.gz"))->data, QByteArray("old"));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/report (1).tar.gz"))->data, QByteArray("older copy"));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/report (2).tar.gz"))->data, QByteArray("new"));
        QCOMPARE(UploadJob::keepBothName(QStringLiteral("a/.bashrc"), 3), QStringLiteral("a/.bashrc (3)"));
        QCOMPARE(UploadJob::keepBothName(QStringLiteral("x"), 1), QStringLiteral("x (1)"));
    }

    void aKeyThatAppearsMeanwhileIsNotOverwritten()
    {
        // Generic profile: conditional writes unknown → HEAD right before.
        writeLocal(QStringLiteral("late.txt"), "mine");
        auto* job = make({m_local + QStringLiteral("/late.txt")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        QCOMPARE(job->preview().conflicts, 0);
        m_objects->putObject(kBucket, QStringLiteral("backup/late.txt"), "someone else's");
        job->confirm();
        QVERIFY(done.wait(30000));
        QCOMPARE(job->report().skipped, 1);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/late.txt"))->data, QByteArray("someone else's"));
    }

    void oneFileFromTheDialogNeedsNoPreview()
    {
        setEnvironment(Environment::Development);
        writeLocal(QStringLiteral("quick.txt"), "fast");
        auto* job = make({m_local + QStringLiteral("/quick.txt")}, QString(), false);
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(20000));
        QVERIFY(preview.isEmpty());
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("quick.txt"))->data, QByteArray("fast"));

        // On production it always asks.
        setEnvironment(Environment::Production);
        auto* prod = make({m_local + QStringLiteral("/quick.txt")}, QStringLiteral("p/"), false);
        QVERIFY(runConfirmed(prod));
        QCOMPARE(preview.size(), 1);
    }

    void aLargeFileGoesMultipartAndLeavesNoRecord()
    {
        const QByteArray big = QByteArray(65 * 1024 * 1024 + 5, 'z');
        writeLocal(QStringLiteral("big.bin"), big);
        auto* job = make({m_local + QStringLiteral("/big.bin")});
        QVERIFY(runConfirmed(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/big.bin"))->size, big.size());
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/big.bin"))->partSizes.size(), 9);
        QCOMPARE(job->md5Verified(), 1);
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        QVERIFY(m_transfers->resumeStore()->ids().isEmpty());
    }

    void cancelWhileUploadingAbortsWhatRuns()
    {
        for (int i = 0; i < 40; ++i)
            writeLocal(QStringLiteral("many/f%1").arg(i), content(i));
        m_objects->setDelay(QStringLiteral("PutObject"), 200);
        auto* job = make({m_local + QStringLiteral("/many")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        job->confirm();
        QTRY_VERIFY(m_objects->count(QStringLiteral("PutObject")) >= 1);
        job->cancel();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Cancelled);
        QVERIFY(m_objects->keys(kBucket).size() < 40);
        QTRY_COMPARE(m_transfers->limiter(m_h->account.id)->inFlight(), 0);
    }

    void pauseHoldsNewFiles()
    {
        for (int i = 0; i < 30; ++i)
            writeLocal(QStringLiteral("p/f%1").arg(i), content(i));
        m_objects->setDelay(QStringLiteral("PutObject"), 50);
        auto* job = make({m_local + QStringLiteral("/p")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        job->confirm();
        QTRY_VERIFY(m_objects->count(QStringLiteral("PutObject")) >= 2);
        job->pause();
        QVERIFY(job->isPaused());
        QTest::qWait(400);   // what was under way finishes
        const int sent = m_objects->count(QStringLiteral("PutObject"));
        QTest::qWait(400);
        QCOMPARE(m_objects->count(QStringLiteral("PutObject")), sent);
        QVERIFY(done.isEmpty());
        job->resume();
        QVERIFY(done.wait(30000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->keys(kBucket).size(), 30);
    }

    void failedFilesCanBeRetried()
    {
        writeLocal(QStringLiteral("r/ok.txt"), "ok");
        writeLocal(QStringLiteral("r/bad.txt"), "bad");
        // Refused every time — a policy the server enforces, not a blip.
        m_h->server->addRoute({"PUT", QRegularExpression(QStringLiteral("bad\\.txt$")), {}, {}},
                              {FakeResponse::s3Error(403, "AccessDenied", "not this one")}, 1);
        auto* job = make({m_local + QStringLiteral("/r")});
        QVERIFY(runConfirmed(job));
        QCOMPARE(job->outcome(), Job::Outcome::PartlyFailed);
        QCOMPARE(job->failedItems().size(), 1);
        QCOMPARE(job->failedItems().first().key, QStringLiteral("backup/r/bad.txt"));
        QVERIFY(job->report().failures.first().contains(QLatin1String("bad.txt")));

        auto* retry = new UploadJob(m_h->account, kBucket, job->failedItems(), m_ops->planDir());
        QVERIFY(runConfirmed(retry));
        QCOMPARE(retry->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/r/bad.txt"))->data, QByteArray("bad"));
    }

    void aReadOnlyAccountUploadsNothing()
    {
        setEnvironment(Environment::Development, true);
        writeLocal(QStringLiteral("x.txt"), "x");
        auto* job = make({m_local + QStringLiteral("/x.txt")});
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(5000));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QVERIFY(m_objects->keys(kBucket).isEmpty());
    }

    void thePreviewOffersThePolicy()
    {
        writeLocal(QStringLiteral("c.txt"), "new");
        m_objects->putObject(kBucket, QStringLiteral("backup/c.txt"), "old");
        auto* job = make({m_local + QStringLiteral("/c.txt")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        OperationPreviewDialog dlg(job);
        QVERIFY(dlg.conflictPolicy());
        dlg.conflictPolicy()->setCurrentIndex(dlg.conflictPolicy()->findData(int(Job::ConflictPolicy::Overwrite)));
        QVERIFY(dlg.productionCheck());   // the harness account is production
        dlg.productionCheck()->setChecked(true);
        dlg.confirmButton()->click();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->conflictPolicy(), Job::ConflictPolicy::Overwrite);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/c.txt"))->data, QByteArray("new"));
    }

    void suspendedUploadContinuesInTheNextSession()
    {
        // 20 small files and one multipart file; the app quits midway.
        for (int i = 0; i < 20; ++i)
            writeLocal(QStringLiteral("trip/f%1.txt").arg(i, 2, 10, QLatin1Char('0')), content(i));
        const QByteArray big(70 * 1024 * 1024 + 9, 'b');
        writeLocal(QStringLiteral("trip/big.bin"), big);
        m_objects->setDelay(QStringLiteral("PutObject"), 60);
        m_objects->setDelay(QStringLiteral("UploadPart"), 60);
        auto* job = make({m_local + QStringLiteral("/trip")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        job->confirm();
        QTRY_VERIFY_WITH_TIMEOUT(m_objects->count(QStringLiteral("UploadPart")) >= 3
                                     && m_objects->count(QStringLiteral("PutObject")) >= 4,
                                 20000);
        job->suspend();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Cancelled);
        QVERIFY(job->isSuspended());
        QVERIFY(m_objects->keys(kBucket).size() < 21);
        QCOMPARE(m_objects->uploadIds(kBucket).size(), 1);   // the big file's upload stays open
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        QVERIFY(m_transfers->jobs()->waitForWrites(5000));

        // The next session: a fresh TransferService over the same directory.
        m_ops->setTransfers(nullptr);
        m_transfers = std::make_unique<TransferService>(m_h->path(QStringLiteral("transfers")), nullptr);
        m_ops->setTransfers(m_transfers.get());
        std::optional<QList<TransferJobStore::Record>> records;
        QObject ctx;
        m_transfers->jobs()->load(&ctx, [&](const QList<TransferJobStore::Record>& r) { records = r; });
        QTRY_VERIFY(records.has_value());
        QCOMPARE(records->size(), 1);
        QVERIFY(records->first().suspended);
        QCOMPARE(records->first().items, 21);

        QTest::qWait(300);   // requests of the first session still in this thread's queue
        QMap<QString, int> putsBefore;
        for (const RecordedRequest& r : m_h->server->requests())
            if (r.method == "PUT" && !r.hasQueryItem("partNumber"))
                ++putsBefore[QString::fromLatin1(r.path)];
        const QStringList finishedBefore = m_objects->keys(kBucket);
        QVERIFY(!finishedBefore.isEmpty());

        m_objects->setDelay(QStringLiteral("PutObject"), 0);
        m_objects->setDelay(QStringLiteral("UploadPart"), 0);
        auto* resumed = new UploadJob(m_h->account, records->first());
        QSignalSpy resumedDone(resumed, &Job::finished);
        m_ops->start(resumed);
        QVERIFY(resumedDone.wait(60000));
        QCOMPARE(resumed->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(m_objects->keys(kBucket).size(), 21);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/trip/big.bin"))->size, big.size());
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("backup/trip/f07.txt"))->data, content(7));
        QVERIFY(m_objects->uploadIds(kBucket).isEmpty());
        // Files finished before the quit were not sent again …
        QMap<QString, int> putsAfter;
        for (const RecordedRequest& r : m_h->server->requests())
            if (r.method == "PUT" && !r.hasQueryItem("partNumber"))
                ++putsAfter[QString::fromLatin1(r.path)];
        for (const QString& key : finishedBefore) {
            const QString path = QStringLiteral("/bkt/") + QString::fromLatin1(QUrl::toPercentEncoding(key, "/"));
            if (key.endsWith(QLatin1String("big.bin")))
                continue;   // multipart: checked by its parts below
            QVERIFY2(putsBefore.value(path) >= 1, qPrintable(path));
            QVERIFY2(putsAfter.value(path) == putsBefore.value(path), qPrintable(key));
        }
        // … and the big file's parts that reached the server neither.
        QMap<int, int> parts;
        for (const auto& [id, n] : m_objects->partUploads())
            ++parts[n];
        QCOMPARE(parts.size(), 9);
        for (auto it = parts.cbegin(); it != parts.cend(); ++it)
            QCOMPARE(it.value(), 1);
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        QVERIFY(m_transfers->jobs()->waitForWrites(5000));
        QVERIFY(m_transfers->resumeStore()->ids().isEmpty());
        std::optional<QList<TransferJobStore::Record>> after;
        m_transfers->jobs()->load(&ctx, [&](const QList<TransferJobStore::Record>& r) { after = r; });
        QTRY_VERIFY(after.has_value());
        QVERIFY(after->isEmpty());
    }

    // QBUCKET_SCREENSHOT_DIR (with a real platform): upload-conflicts.png.
    void screenshotOfTheConflictPreview()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        QTranslator plurals;   // as the app does
        if (plurals.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")))
            QCoreApplication::installTranslator(&plurals);
        for (int i = 0; i < 6; ++i)
            writeLocal(QStringLiteral("site/page%1.html").arg(i), content(i, 2000 + 300 * i));
        m_objects->putObject(kBucket, QStringLiteral("backup/site/page1.html"), "old");
        m_objects->putObject(kBucket, QStringLiteral("backup/site/page4.html"), "older");
        auto* job = make({m_local + QStringLiteral("/site")});
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        m_ops->start(job);
        QVERIFY(preview.wait(20000));
        OperationPreviewDialog dlg(job);
        dlg.show();
        QTest::qWait(300);
        QDir().mkpath(dir);
        QVERIFY(dlg.grab().save(QDir(dir).filePath(QStringLiteral("upload-conflicts.png"))));
        dlg.reject();
    }

    void emptySelectionUploadsNothing()
    {
        QDir().mkpath(m_local + QStringLiteral("/hollow"));
        m_objects->putObject(kBucket, QStringLiteral("backup/hollow/"), QByteArray());   // the marker exists
        auto* job = make({m_local + QStringLiteral("/hollow")});
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().verification, QStringLiteral("nothing to upload"));
    }
};

QTEST_MAIN(TestUploadJob)
#include "test_upload_job.moc"
