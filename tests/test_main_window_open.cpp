// "Open" end to end through the real main window against FakeObjectStore:
// activating an object downloads and opens it (the opener records), a
// change shows the bar, Upload replaces the object through the gate, and
// quitting with an unsent change asks first.

#include "controllers/BrowseController.h"
#include "controllers/OpenFileService.h"
#include "controllers/SettingsController.h"
#include "support/AccountHarness.h"
#include "ui/MainWindow.h"
#include "ui/ObjectBrowser.h"
#include "ui/OpenFilesDialog.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QDir>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableView>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestMainWindowOpen : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<SettingsController> m_settings;
    std::unique_ptr<ThemeManager> m_theme;
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<MainWindow> m_window;
    ObjectBrowser* m_browser = nullptr;
    QList<QUrl> m_opened;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("qbucket-test-open"));
        QCoreApplication::setOrganizationName(QStringLiteral("qbucket-test"));
        m_settings = std::make_unique<SettingsController>();
        m_theme = std::make_unique<ThemeManager>();
        m_theme->apply(QStringLiteral("Dark"));
    }

    void init()
    {
        m_h = std::make_unique<AccountHarness>(QStringLiteral("Dev"));
        QVERIFY(m_h->ok());
        Account a = m_h->account;
        a.providerId = QStringLiteral("aws");
        a.addressing = s3::AddressingStyle::Path;
        a.environment = Environment::Development;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(QStringLiteral("bkt"));
        m_objects->setVersioning(QStringLiteral("bkt"), QStringLiteral("Enabled"));
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("notes.txt"), "draft\n", QStringLiteral("text/plain"));
        m_window = std::make_unique<MainWindow>(m_settings.get(), m_theme.get(), m_h->accounts.get(), m_h->secrets.get());
        m_window->resize(1100, 700);
        m_window->show();
        m_opened.clear();
        m_window->openFiles()->setOpener([this](const QUrl& url) {
            m_opened.append(url);
            return true;
        });
        m_window->openFiles()->setSettleDelay(50);
        m_browser = m_window->findChild<ObjectBrowser*>();
        QVERIFY(m_browser);
        m_browser->openBucket(m_h->account.id, QStringLiteral("bkt"));
        QTRY_COMPARE_WITH_TIMEOUT(m_browser->model()->state(), ObjectTableModel::State::Complete, 10000);
    }

    void cleanup()
    {
        // Nothing to ask about on the way out.
        for (const auto& e : m_window->openFiles()->entries())
            m_window->openFiles()->discard(e.id);
        m_window.reset();
        m_objects.reset();
        m_h.reset();
    }

    void openEditAndUpload()
    {
        m_browser->view()->setFocus();
        m_browser->view()->selectRow(0);
        QTRY_VERIFY(m_browser->openAction()->isEnabled());
        m_browser->openAction()->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(m_opened.size(), 1, 10000);
        const QString path = m_opened.first().toLocalFile();
        QVERIFY(path.endsWith(QLatin1String("/notes.txt")));
        QVERIFY(m_window->changeBar()->isHidden());

        // Edited in the other application.
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("final text\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->changeBar()->isHidden(), 5000);
        QVERIFY(m_window->changeBar()->findChild<QLabel*>()->text().contains(QLatin1String("notes.txt")));
        if (const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR"); !dir.isEmpty()) {
            m_window->resize(1280, 760);
            QTest::qWait(150);
            QDir().mkpath(dir);
            QVERIFY(m_window->grab().save(QDir(dir).filePath(QStringLiteral("open-changed.png"))));
        }
        QPushButton* upload = nullptr;
        for (QPushButton* b : m_window->changeBar()->findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("Upload"))
                upload = b;
        }
        QVERIFY(upload);
        if (!upload)
            return;
        upload->click();
        // Versioned bucket, development account: no preview needed.
        QTRY_COMPARE_WITH_TIMEOUT(m_objects->object(QStringLiteral("bkt"), QStringLiteral("notes.txt"))->data,
                                  QByteArray("final text\n"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->changeBar()->isHidden(), 5000);
        QCOMPARE(m_objects->object(QStringLiteral("bkt"), QStringLiteral("notes.txt"))->contentType,
                 QStringLiteral("text/plain"));
    }

    void theOpenFilesListOffersWhatFits()
    {
        m_browser->view()->selectRow(0);
        m_browser->openAction()->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(m_opened.size(), 1, 10000);
        QAction* list = nullptr;
        for (QAction* a : m_window->findChildren<QAction*>()) {
            if (a->text() == QStringLiteral("&Open Files…"))
                list = a;
        }
        QVERIFY(list && list->isEnabled());
        if (!list)
            return;
        list->trigger();
        QTRY_VERIFY(m_window->findChild<OpenFilesDialog*>() != nullptr);
        auto* dlg = m_window->findChild<OpenFilesDialog*>();
        QCOMPARE(dlg->table()->rowCount(), 1);
        QCOMPARE(dlg->table()->item(0, 0)->text(), QStringLiteral("notes.txt"));
        if (const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(dlg->grab().save(QDir(dir).filePath(QStringLiteral("open-files.png"))));
        }
        QVERIFY(!dlg->uploadButton()->isEnabled());   // nothing changed
        QVERIFY(dlg->discardButton()->isEnabled());
        dlg->discardButton()->click();   // unchanged: closed without asking
        QTRY_VERIFY(m_window->openFiles()->entries().isEmpty());
        QVERIFY(!QFile::exists(m_opened.first().toLocalFile()));
        dlg->close();
    }

    void quittingWithAnUnsentChangeAsks()
    {
        m_browser->view()->selectRow(0);
        m_browser->openAction()->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(m_opened.size(), 1, 10000);
        {
            QFile f(m_opened.first().toLocalFile());
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("unsent\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->changeBar()->isHidden(), 5000);
        m_window->close();
        QTRY_VERIFY(m_window->findChild<QMessageBox*>() != nullptr);
        auto* box = m_window->findChild<QMessageBox*>();
        QVERIFY(box);
        QVERIFY(m_window->isVisible());   // not closed unasked
        box->button(QMessageBox::Cancel)->click();
        QTest::qWait(100);
        QVERIFY(m_window->isVisible());
        QCOMPARE(m_window->openFiles()->changedEntries().size(), 1);
    }
};

QTEST_MAIN(TestMainWindowOpen)
#include "test_main_window_open.moc"
