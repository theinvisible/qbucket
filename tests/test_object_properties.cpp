// ObjectPropertyJob (an object's tags, its canned ACL) through the
// OperationController against FakeObjectStore, and the DetailPane's Tags and
// Access tabs that start it.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CopyJob.h"
#include "controllers/Journal.h"
#include "controllers/ObjectPropertyJob.h"
#include "controllers/OperationController.h"
#include "models/ObjectTableModel.h"
#include "support/AccountHarness.h"
#include "ui/DetailPane.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QComboBox>
#include <QDir>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTranslator>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {
const QString kBucket = QStringLiteral("bkt");
} // namespace

class TestObjectProperties : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
    QList<Job::Preview> m_previews;
    std::function<void(Job*)> m_beforeConfirm;
    ThemeManager m_theme;

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    bool run(Job* job)
    {
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        return done.wait(30000) || done.size() == 1;
    }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        useProvider(QStringLiteral("aws"));
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_objects->putObject(kBucket, QStringLiteral("doc.txt"), "content");
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_previews.clear();
        m_beforeConfirm = nullptr;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            if (m_beforeConfirm)
                m_beforeConfirm(job);
            job->confirm();
        });
    }

    void cleanup()
    {
        m_ops.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void setsTagsAndChecksThem()
    {
        auto* job = ObjectPropertyJob::tags(m_h->account, kBucket, QStringLiteral("doc.txt"),
                                            {{QStringLiteral("project"), QStringLiteral("q")}});
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(job->report().verified);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("doc.txt"))->tags.size(), 1);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("doc.txt"))->data, QByteArray("content"));
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("project=q")));
        // Removing them all: an empty set.
        QVERIFY(run(ObjectPropertyJob::tags(m_h->account, kBucket, QStringLiteral("doc.txt"), {})));
        QVERIFY(m_objects->object(kBucket, QStringLiteral("doc.txt"))->tags.isEmpty());
    }

    void setsACannedAcl()
    {
        auto* job = ObjectPropertyJob::acl(m_h->account, kBucket, QStringLiteral("doc.txt"), QStringLiteral("public-read"));
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(job->report().verified);
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("doc.txt"))->acl, QStringLiteral("public-read"));
        QVERIFY(m_previews.first().warnings.join(u' ').contains(QLatin1String("Anyone")));
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("Now: private")));
    }

    // A generic server: tag / ACL writes aren't confirmed — refused, nothing sent.
    void unconfirmedWritesAreRefused()
    {
        useProvider(QStringLiteral("generic"));
        auto* job = ObjectPropertyJob::tags(m_h->account, kBucket, QStringLiteral("doc.txt"),
                                            {{QStringLiteral("k"), QStringLiteral("v")}});
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Refused);
        QVERIFY(job->errorText().contains(QLatin1String("tags and ACLs")));
        QCOMPARE(m_objects->count(QStringLiteral("PutObjectTagging")), 0);
    }

    // A proxy that drops the subresource stores the request as the object:
    // the answer (an ETag) gives it away — a failure, never "done".
    void aWriteTakenForAnUploadFails()
    {
        m_objects->setDropObjectSubresources(true);
        auto* job = ObjectPropertyJob::acl(m_h->account, kBucket, QStringLiteral("doc.txt"), QStringLiteral("private"));
        QVERIFY(run(job));
        QVERIFY(job->outcome() == Job::Outcome::Failed || job->outcome() == Job::Outcome::Refused);
        QVERIFY(!job->report().verified);
    }

    void anObjectChangedBeforeTheWriteIsLeftAlone()
    {
        m_beforeConfirm = [this](Job*) { m_objects->putObject(kBucket, QStringLiteral("doc.txt"), "rewritten"); };
        auto* job = ObjectPropertyJob::tags(m_h->account, kBucket, QStringLiteral("doc.txt"),
                                            {{QStringLiteral("k"), QStringLiteral("v")}});
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY(job->report().failures.first().contains(QLatin1String("changed since it was planned")));
        QVERIFY(m_objects->object(kBucket, QStringLiteral("doc.txt"))->tags.isEmpty());
    }

    // The Tags and Access tabs: shown when visible, written through the gate.
    void theDetailPaneEditsTagsAndAccess()
    {
        m_objects->setObjectTags(kBucket, QStringLiteral("doc.txt"), {{QStringLiteral("old"), QStringLiteral("1")}});
        ObjectTableModel model(m_h->sessions.get());
        model.setLocation(m_h->account.id, kBucket, QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), ObjectTableModel::State::Complete, 10000);
        DetailPane pane(m_h->sessions.get());
        pane.setControllers(m_ops.get(), m_caps.get());
        pane.show();
        QSignalSpy head(&pane, &DetailPane::headFinished);
        QSignalSpy tags(&pane, &DetailPane::tagsLoaded);
        pane.tabs()->setCurrentIndex(DetailPane::TagsTab);
        pane.setSelection(m_h->account.id, kBucket, {model.index(0, 0)});
        QVERIFY(head.wait(10000));
        QVERIFY(tags.size() == 1 || tags.wait(10000));
        QCOMPARE(pane.tagTable()->rowCount(), 1);
        QCOMPARE(pane.tagTable()->item(0, 0)->text(), QStringLiteral("old"));
        QTRY_VERIFY(pane.applyTagsButton()->isEnabled());
        pane.tagTable()->item(0, 1)->setText(QStringLiteral("2"));
        tags.clear();
        pane.applyTagsButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_objects->object(kBucket, QStringLiteral("doc.txt"))->tags.value(0).value == QLatin1String("2"),
                                 10000);
        QVERIFY(tags.size() == 1 || tags.wait(10000));   // shown afresh afterwards

        QSignalSpy access(&pane, &DetailPane::accessLoaded);
        pane.tabs()->setCurrentIndex(DetailPane::AccessTab);
        QVERIFY(access.size() == 1 || access.wait(10000));
        QCOMPARE(pane.cannedAclBox()->currentText(), QStringLiteral("private"));
        QVERIFY(pane.grantList()->topLevelItemCount() >= 1);
        pane.cannedAclBox()->setCurrentText(QStringLiteral("public-read"));
        pane.applyAccessButton()->click();
        QTRY_COMPARE_WITH_TIMEOUT(m_objects->object(kBucket, QStringLiteral("doc.txt"))->acl, QStringLiteral("public-read"), 10000);
    }

    // QBUCKET_SCREENSHOT_DIR (with a real platform): details-tags.png,
    // details-access.png and copy-preview.png (devlog).
    void screenshot()
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("QBUCKET_SCREENSHOT_DIR not set");
        QTranslator plurals;   // as the app does
        if (plurals.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")))
            QCoreApplication::installTranslator(&plurals);
        m_theme.apply(QStringLiteral("Dark"));
        QDir().mkpath(dir);
        m_objects->putObject(kBucket, QStringLiteral("reports/q3.pdf"), QByteArray(48000, 'p'), QStringLiteral("application/pdf"));
        m_objects->setObjectTags(kBucket, QStringLiteral("reports/q3.pdf"),
                                 {{QStringLiteral("project"), QStringLiteral("finance")}, {QStringLiteral("retention"), QStringLiteral("7y")},
                                  {QStringLiteral("owner"), QStringLiteral("controlling")}});
        m_objects->setObjectAcl(kBucket, QStringLiteral("reports/q3.pdf"), QStringLiteral("public-read"));
        ObjectTableModel model(m_h->sessions.get());
        model.setLocation(m_h->account.id, kBucket, QStringLiteral("reports/"));
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), ObjectTableModel::State::Complete, 10000);
        DetailPane pane(m_h->sessions.get());
        pane.setControllers(m_ops.get(), m_caps.get());
        pane.resize(440, 520);
        pane.show();
        QSignalSpy tags(&pane, &DetailPane::tagsLoaded);
        pane.tabs()->setCurrentIndex(DetailPane::TagsTab);
        pane.setSelection(m_h->account.id, kBucket, {model.index(0, 0)});
        QVERIFY(tags.wait(10000));
        QTest::qWait(400);
        QVERIFY(pane.grab().save(QDir(dir).filePath(QStringLiteral("details-tags.png"))));
        QSignalSpy access(&pane, &DetailPane::accessLoaded);
        pane.tabs()->setCurrentIndex(DetailPane::AccessTab);
        QVERIFY(access.wait(10000));
        QTest::qWait(400);
        QVERIFY(pane.grab().save(QDir(dir).filePath(QStringLiteral("details-access.png"))));

        // A copy with conflicts into another bucket: the preview.
        m_objects->createBucket(QStringLiteral("archive"));
        for (const char* k : {"reports/q1.pdf", "reports/q2.pdf", "reports/q4-draft.pdf"})
            m_objects->putObject(kBucket, QString::fromLatin1(k), QByteArray(12000, 'x'));
        m_objects->putObject(QStringLiteral("archive"), QStringLiteral("2026/reports/q1.pdf"), QByteArray(9000, 'o'));
        m_objects->putObject(QStringLiteral("archive"), QStringLiteral("2026/reports/q2.pdf"), QByteArray(9500, 'o'));
        disconnect(m_ops.get(), &OperationController::previewNeeded, this, nullptr);
        OperationPreviewDialog* dlg = nullptr;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [&dlg](Job* job) {
            dlg = new OperationPreviewDialog(job);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
        });
        auto* copy = new CopyJob(m_h->account, kBucket, {{QStringLiteral("reports/"), true}}, QString(),
                                 QStringLiteral("archive"), QStringLiteral("2026/"), CopyJob::Mode::Move, m_h->path(QStringLiteral("plans")));
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        m_ops->start(copy);
        QTRY_VERIFY_WITH_TIMEOUT(dlg != nullptr, 10000);
        QTest::qWait(500);
        QVERIFY(dlg->grab().save(QDir(dir).filePath(QStringLiteral("copy-preview.png"))));
        dlg->reject();
        m_theme.apply(QStringLiteral("Light"));
    }

    // A server without object tags: the tab is hidden once that is known.
    void tabsOfMissingFeaturesAreHidden()
    {
        m_h->server->addRoute({"GET", {}, {"tagging"}, {}}, {FakeResponse::s3Error(501, "NotImplemented")});
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY(m_caps->isLoaded(m_h->account.id));
        m_caps->bucketOpened(m_h->account.id, kBucket);
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_h->account.id, kBucket).has_value(), 10000);
        ObjectTableModel model(m_h->sessions.get());
        model.setLocation(m_h->account.id, kBucket, QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), ObjectTableModel::State::Complete, 10000);
        DetailPane pane(m_h->sessions.get());
        pane.setControllers(m_ops.get(), m_caps.get());
        pane.setSelection(m_h->account.id, kBucket, {model.index(0, 0)});
        QVERIFY(!pane.tabs()->isTabVisible(DetailPane::TagsTab));
        QVERIFY(pane.tabs()->isTabVisible(DetailPane::AccessTab));
    }
};

QTEST_MAIN(TestObjectProperties)
#include "test_object_properties.moc"
