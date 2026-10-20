// TransferModel and TransferPanel with a real UploadJob against
// FakeObjectStore: rows, progress, pause / resume, the finished summary,
// retry of what failed, clear finished.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"
#include "models/TransferModel.h"
#include "support/AccountHarness.h"
#include "ui/ThemeManager.h"
#include "ui/TransferPanel.h"

#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTableView>
#include <QToolButton>
#include <QTranslator>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestTransferPanel : public QObject {
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
    std::unique_ptr<TransferModel> m_model;
    ThemeManager m_theme;

    QString writeLocal(const QString& rel, const QByteArray& data)
    {
        QString path = m_h->path(QStringLiteral("local/") + rel);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(data);
        return path;
    }

    UploadJob* startUpload(const QStringList& sources)
    {
        auto* job = new UploadJob(m_h->account, QStringLiteral("bkt"), QString(), sources, m_ops->planDir(), {});
        connect(m_ops.get(), &OperationController::previewNeeded, job, [job](Job* j) {
            if (j == job)
                j->confirm();
        });
        m_ops->start(job);
        return job;
    }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Dark")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(QStringLiteral("bkt"));
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
        m_model = std::make_unique<TransferModel>(m_ops.get());
    }

    void cleanup()
    {
        QVERIFY(s3::transfer::flushResumeWrites(5000));
        m_model.reset();
        m_ops.reset();
        m_transfers.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void aTransferRunsThroughThePanel()
    {
        TransferPanel panel(m_model.get());
        panel.resize(900, 300);
        panel.show();
        QVERIFY(!panel.clearButton()->isEnabled());
        // Long enough (≥ 1.5 s) to be paused while it runs, also on a busy machine.
        for (int i = 0; i < 12; ++i)
            writeLocal(QStringLiteral("set/f%1").arg(i), QByteArray(20000, char('a' + i)));
        m_objects->setDelay(QStringLiteral("PutObject"), 500);
        QSignalSpy started(m_model.get(), &TransferModel::transferStarted);
        UploadJob* job = startUpload({m_h->path(QStringLiteral("local/set"))});
        QSignalSpy done(job, &Job::finished);
        QCOMPARE(started.size(), 1);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->index(0, 0).data(TransferModel::KindRole).toInt(), int(Job::TransferKind::Upload));
        QCOMPARE(panel.view()->selectionModel()->selectedRows().size(), 1);   // the newest is selected

        QTRY_VERIFY_WITH_TIMEOUT(m_model->index(0, 0).data(TransferModel::CanPauseRole).toBool(), 20000);
        QVERIFY(panel.pauseButton()->isEnabled());   // the phase change reached the panel at once
        panel.pauseButton()->click();
        QVERIFY(job->isPaused());
        QTRY_VERIFY(panel.resumeButton()->isEnabled());
        QTRY_COMPARE(m_model->index(0, TransferModel::StateColumn).data().toString(), QStringLiteral("paused"));
        panel.resumeButton()->click();
        QVERIFY(!job->isPaused());
        QTRY_VERIFY_WITH_TIMEOUT(m_model->index(0, 0).data(TransferModel::ProgressRole).toInt() > 0, 20000);
        QVERIFY(done.size() == 1 || done.wait(30000));   // may have ended while we looked

        QTRY_VERIFY(m_model->index(0, 0).data(TransferModel::FinishedRole).toBool());
        QCOMPARE(m_model->index(0, 0).data(TransferModel::ProgressRole).toInt(), 1000);
        QVERIFY(!m_model->index(0, TransferModel::StateColumn).data().toString().isEmpty());
        QVERIFY(!panel.cancelButton()->isEnabled());
        QVERIFY(!panel.retryButton()->isEnabled());   // nothing failed
        QVERIFY(panel.clearButton()->isEnabled());
        panel.clearButton()->click();
        QCOMPARE(m_model->rowCount(), 0);
    }

    void failedFilesCanBeRetriedFromThePanel()
    {
        TransferPanel panel(m_model.get());
        writeLocal(QStringLiteral("r/ok"), "ok");
        writeLocal(QStringLiteral("r/no"), "no");
        m_h->server->addRoute({"PUT", QRegularExpression(QStringLiteral("/no$")), {}, {}},
                              {FakeResponse::s3Error(403, "AccessDenied")}, 1);
        UploadJob* job = startUpload({m_h->path(QStringLiteral("local/r"))});
        QSignalSpy done(job, &Job::finished);
        QVERIFY(done.size() == 1 || done.wait(30000));
        QTRY_VERIFY(m_model->index(0, 0).data(TransferModel::CanRetryRole).toBool());
        panel.view()->selectRow(0);
        QTRY_VERIFY(panel.retryButton()->isEnabled());
        QSignalSpy started(m_model.get(), &TransferModel::transferStarted);
        connect(m_ops.get(), &OperationController::previewNeeded, this, [](Job* j) { j->confirm(); });
        panel.retryButton()->click();
        QCOMPARE(started.size(), 1);
        QCOMPARE(m_model->rowCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(m_model->index(1, 0).data(TransferModel::FinishedRole).toBool(), 30000);
        QCOMPARE(m_objects->object(QStringLiteral("bkt"), QStringLiteral("r/no"))->data, QByteArray("no"));
    }

    void cancelFromThePanel()
    {
        TransferPanel panel(m_model.get());
        for (int i = 0; i < 20; ++i)
            writeLocal(QStringLiteral("c/f%1").arg(i), QByteArray(1000, 'c'));
        m_objects->setDelay(QStringLiteral("PutObject"), 200);
        UploadJob* job = startUpload({m_h->path(QStringLiteral("local/c"))});
        QSignalSpy done(job, &Job::finished);
        QTRY_VERIFY_WITH_TIMEOUT(m_objects->count(QStringLiteral("PutObject")) >= 1, 20000);
        QTRY_VERIFY(panel.cancelButton()->isEnabled());
        panel.cancelButton()->click();
        QVERIFY(done.size() == 1 || done.wait(20000));
        QTRY_VERIFY(m_model->index(0, 0).data(TransferModel::FinishedRole).toBool());
        QCOMPARE(m_model->runningCount(), 0);
    }

    // QBUCKET_SCREENSHOT_DIR (with a real platform): transfers-<theme>.png.
    void screenshot()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        QTranslator plurals;   // as the app does
        if (plurals.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")))
            QCoreApplication::installTranslator(&plurals);
        for (const QString& themeName : {QStringLiteral("Dark"), QStringLiteral("Light")}) {
            m_theme.apply(themeName);
            init();
            TransferPanel panel(m_model.get());
            panel.resize(1100, 230);
            panel.show();
            // A finished upload, one that partly failed, one running.
            for (int i = 0; i < 5; ++i)
                writeLocal(QStringLiteral("photos/IMG_%1.jpg").arg(4400 + i), QByteArray(300000, 'p'));
            Job* done = startUpload({m_h->path(QStringLiteral("local/photos"))});
            QSignalSpy doneSpy(done, &Job::finished);
            QVERIFY(doneSpy.size() == 1 || doneSpy.wait(20000));
            writeLocal(QStringLiteral("reports/q3.pdf"), "pdf");
            writeLocal(QStringLiteral("reports/locked.xlsx"), "xls");
            m_h->server->addRoute({"PUT", QRegularExpression(QStringLiteral("locked\\.xlsx$")), {}, {}},
                                  {FakeResponse::s3Error(403, "AccessDenied")}, 1);
            Job* partly = startUpload({m_h->path(QStringLiteral("local/reports"))});
            QSignalSpy partlySpy(partly, &Job::finished);
            QVERIFY(partlySpy.size() == 1 || partlySpy.wait(20000));
            for (int i = 0; i < 30; ++i)
                writeLocal(QStringLiteral("backup/part-%1.bin").arg(i, 3, 10, QLatin1Char('0')), QByteArray(400000, 'b'));
            m_objects->setDelay(QStringLiteral("PutObject"), 1500);
            startUpload({m_h->path(QStringLiteral("local/backup"))});
            QTest::qWait(3200);
            panel.view()->selectRow(2);
            QTest::qWait(600);
            QDir().mkpath(dir);
            QVERIFY(panel.grab().save(QDir(dir).filePath(QStringLiteral("transfers-%1.png").arg(themeName.toLower()))));
            cleanup();
        }
        init();
    }

    void formats()
    {
        QCOMPARE(TransferModel::formatSpeed(0), QString());
        QVERIFY(TransferModel::formatSpeed(1.5e6).endsWith(QLatin1String("/s")));
        QCOMPARE(TransferModel::formatRemaining(-1), QString());
        QVERIFY(TransferModel::formatRemaining(42).contains(QLatin1String("42")));
        QVERIFY(TransferModel::formatRemaining(7300).startsWith(QLatin1String("2 h")));
    }
};

QTEST_MAIN(TestTransferPanel)
#include "test_transfer_panel.moc"
