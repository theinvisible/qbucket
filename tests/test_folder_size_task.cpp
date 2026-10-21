// FolderSizeTask: sums over pages and storage classes (markers left out),
// cancel, an error that stays an error; the DetailPane's "Calculate Size".

#include "controllers/FolderSizeTask.h"
#include "models/ObjectTableModel.h"
#include "support/AccountHarness.h"
#include "ui/DetailPane.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/Session.h>
#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestFolderSizeTask : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    ThemeManager m_theme;

    s3::Session* session() { return *m_h->sessions->session(m_h->account.id); }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(QStringLiteral("bkt"));
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("a/"), QByteArray());   // a marker
        for (int i = 0; i < 2500; ++i) {
            const QString k = QStringLiteral("a/f%1").arg(i, 4, 10, QLatin1Char('0'));
            m_objects->putObject(QStringLiteral("bkt"), k, QByteArray(i % 10, 'x'));
            if (i % 5 == 0)
                m_objects->setObjectStorageClass(QStringLiteral("bkt"), k, QStringLiteral("STANDARD_IA"));
        }
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("b/one"), "12345");
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("c/other"), "not counted");
    }

    void cleanup()
    {
        m_objects.reset();
        m_h.reset();
    }

    void sumsEveryPage()
    {
        FolderSizeTask task(session(), QStringLiteral("bkt"), {QStringLiteral("a/"), QStringLiteral("b/")});
        QSignalSpy progress(&task, &FolderSizeTask::progress);
        QSignalSpy done(&task, &FolderSizeTask::finished);
        task.start();
        QVERIFY(done.wait(20000));
        QVERIFY(task.isComplete());
        QCOMPARE(task.totals().objects, 2501);   // the marker isn't an object of its own
        qint64 bytes = 5;
        for (int i = 0; i < 2500; ++i)
            bytes += i % 10;
        QCOMPARE(task.totals().bytes, bytes);
        QCOMPARE(task.totals().byClass.value(QStringLiteral("STANDARD_IA")).first, 500);
        QCOMPARE(task.totals().byClass.value(QStringLiteral("STANDARD")).first, 2001);
        QVERIFY(progress.size() >= 4);   // three pages of a/, one of b/
    }

    void cancelStops()
    {
        m_objects->setDelay(QStringLiteral("ListObjectsV2"), 300);
        FolderSizeTask task(session(), QStringLiteral("bkt"), {QStringLiteral("a/")});
        QSignalSpy progress(&task, &FolderSizeTask::progress);
        QSignalSpy done(&task, &FolderSizeTask::finished);
        task.start();
        QVERIFY(progress.wait(10000));
        task.cancel();
        QCOMPARE(done.size(), 1);
        QVERIFY(!task.isComplete());
        QVERIFY(!task.isRunning());
        const int pages = task.totals().pages;
        QTest::qWait(800);
        QCOMPARE(task.totals().pages, pages);   // nothing counted after the cancel
    }

    // An error is never "0 objects".
    void anErrorStaysAnError()
    {
        m_h->server->addRoute({"GET", QRegularExpression(QStringLiteral("^/bkt")), {"list-type"}, {}},
                              {FakeResponse::s3Error(403, "AccessDenied")});
        FolderSizeTask task(session(), QStringLiteral("bkt"), {QStringLiteral("a/")});
        QSignalSpy done(&task, &FolderSizeTask::finished);
        task.start();
        QVERIFY(done.wait(10000));
        QVERIFY(!task.isComplete());
        QVERIFY(!task.errorText().isEmpty());
    }

    void theDetailPaneCountsOnRequest()
    {
        ObjectTableModel model(m_h->sessions.get());
        model.setLocation(m_h->account.id, QStringLiteral("bkt"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(model.state(), ObjectTableModel::State::Complete, 10000);
        DetailPane pane(m_h->sessions.get());
        pane.show();
        pane.setSelection(m_h->account.id, QStringLiteral("bkt"), {model.index(1, 0)});   // b/
        QVERIFY(pane.sizeButton());
        QCOMPARE(pane.sizeLabel()->text(), QStringLiteral("not counted"));   // never on its own
        pane.sizeButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(pane.sizeLabel()->text().startsWith(QLatin1String("1 object")), 10000);
        // Another selection: the count of the old one stops and its row is gone.
        pane.setSelection(m_h->account.id, QStringLiteral("bkt"), {model.index(0, 0)});
        QVERIFY(pane.sizeLabel());
        QCOMPARE(pane.sizeLabel()->text(), QStringLiteral("not counted"));
    }
};

QTEST_MAIN(TestFolderSizeTask)
#include "test_folder_size_task.moc"
