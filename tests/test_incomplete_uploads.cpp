// The "Incomplete Uploads" dialog: MultipartUploadModel lists what was
// started and never finished, marks what this computer can continue, and
// AbortUploadsJob removes the selected ones through the gate.

#include "accounts/CapabilityStore.h"
#include "controllers/AbortUploadsJob.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "models/MultipartUploadModel.h"
#include "support/AccountHarness.h"
#include "ui/IncompleteUploadsDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QtTest>

#include <optional>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestIncompleteUploads : public QObject {
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
    ThemeManager m_theme;

    QString startUpload(const QString& key)
    {
        s3::CreateMultipartUploadParams p;
        p.bucket = QStringLiteral("bkt");
        p.key = key;
        std::optional<s3::Result<s3::CreateMultipartUploadResult>> r;
        QObject ctx;
        (*m_h->sessions->session(m_h->account.id))->createMultipartUpload(p, &ctx, [&](s3::Result<s3::CreateMultipartUploadResult> x) {
            r.emplace(std::move(x));
        });
        if (!QTest::qWaitFor([&] { return r.has_value(); }, 10000) || !*r)
            return {};
        return (*r)->uploadId;
    }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

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
        m_ops->setTransfers(m_transfers.get());
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

    void listsMarksAndAborts()
    {
        const QString mine = startUpload(QStringLiteral("video/mine.mp4"));
        const QString other = startUpload(QStringLiteral("backup/someone-else.tar"));
        QVERIFY(!mine.isEmpty() && !other.isEmpty());
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("video/mine.mp4"), "an older, finished version");
        // This computer has the records to continue "mine".
        s3::transfer::TransferRecord rec;
        rec.id = QStringLiteral("job1-0");
        rec.direction = s3::transfer::Direction::Upload;
        rec.bucket = QStringLiteral("bkt");
        rec.key = QStringLiteral("video/mine.mp4");
        rec.uploadId = mine;
        rec.partCount = 3;
        rec.partSize = Q_INT64_C(8) * 1024 * 1024;
        QVERIFY(m_transfers->resumeStore()->begin(rec));

        IncompleteUploadsDialog dlg(m_ops.get(), m_transfers.get(), m_h->account.id, QStringLiteral("bkt"));
        dlg.show();
        MultipartUploadModel* model = dlg.model();
        QTRY_COMPARE_WITH_TIMEOUT(model->state(), MultipartUploadModel::State::Ready, 10000);
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->index(0, MultipartUploadModel::KeyColumn).data().toString(), QStringLiteral("backup/someone-else.tar"));
        QTRY_VERIFY(model->index(1, 0).data(MultipartUploadModel::ResumableRole).toBool());
        QVERIFY(!model->index(0, 0).data(MultipartUploadModel::ResumableRole).toBool());
        QVERIFY(!dlg.abortButton()->isEnabled());   // nothing selected

        dlg.view()->selectAll();
        QTRY_VERIFY(dlg.abortButton()->isEnabled());
        QSignalSpy preview(m_ops.get(), &OperationController::previewNeeded);
        dlg.abortButton()->click();
        QVERIFY(preview.wait(10000));
        Job* job = preview.first().at(0).value<Job*>();
        QCOMPARE(job->preview().objects, 2);
        QVERIFY(job->isDestructive());
        QSignalSpy done(job, &Job::finished);
        job->confirm();
        QVERIFY(done.wait(20000));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(m_objects->uploadIds(QStringLiteral("bkt")).isEmpty());
        // The finished object of the same name is untouched.
        QCOMPARE(m_objects->object(QStringLiteral("bkt"), QStringLiteral("video/mine.mp4"))->data,
                 QByteArray("an older, finished version"));
        // The useless resume record went with it, the list was reloaded.
        QTRY_VERIFY(m_transfers->resumeStore()->ids().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 0, 10000);
    }

    void aReadOnlyAccountMayLookButNotAbort()
    {
        Account ro = m_h->account;
        ro.readOnly = true;
        QVERIFY(m_h->accounts->save(ro));
        QVERIFY(!startUpload(QStringLiteral("x")).isEmpty());
        IncompleteUploadsDialog dlg(m_ops.get(), m_transfers.get(), m_h->account.id, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(dlg.model()->state(), MultipartUploadModel::State::Ready, 10000);
        QCOMPARE(dlg.model()->rowCount(), 1);
        dlg.view()->selectAll();
        QVERIFY(!dlg.abortButton()->isEnabled());
        QVERIFY(dlg.abortButton()->toolTip().contains(QLatin1String("read-only")));
    }
};

QTEST_MAIN(TestIncompleteUploads)
#include "test_incomplete_uploads.moc"
