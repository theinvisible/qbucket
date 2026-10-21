// PresignService (the policy: GET on read-only accounts, PUT only where the
// account writes, warnings, the journal) and PresignDialog; a GET link
// fetched without credentials from a server that checks the signature.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/PresignService.h"
#include "support/AccountHarness.h"
#include "ui/PresignDialog.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QUrlQuery>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

class TestPresign : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
    std::unique_ptr<PresignService> m_service;
    ThemeManager m_theme;

    QList<PresignService::Link> sign(const PresignService::Request& r)
    {
        std::optional<QList<PresignService::Link>> out;
        m_service->presign(r, this, [&](const QList<PresignService::Link>& links) { out = links; });
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 10000))
            return {};
        return *out;
    }

    void setReadOnly(bool readOnly)
    {
        Account a = m_h->account;
        a.readOnly = readOnly;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(QStringLiteral("bkt"));
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("docs/report ä.pdf"), "the report");
        m_objects->putObject(QStringLiteral("bkt"), QStringLiteral("docs/b.txt"), "b");
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_service = std::make_unique<PresignService>(m_ops.get());
    }

    void cleanup()
    {
        m_service.reset();
        m_ops.reset();
        m_journal.reset();
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_objects.reset();
        m_h.reset();
    }

    void readOnlyAccountsGetReadLinksOnly()
    {
        setReadOnly(true);
        QVERIFY(m_service->check(m_h->account.id, QStringLiteral("bkt"), "GET").allowed);
        const OperationController::Check put = m_service->check(m_h->account.id, QStringLiteral("bkt"), "PUT");
        QVERIFY(!put.allowed);
        QVERIFY(put.reason.contains(QLatin1String("read-only")));
        PresignService::Request r;
        r.account = m_h->account.id;
        r.bucket = QStringLiteral("bkt");
        r.keys = {QStringLiteral("docs/b.txt")};
        r.method = "PUT";
        const QList<PresignService::Link> links = sign(r);
        QCOMPARE(links.size(), 1);
        QVERIFY(!links.first().url.isValid());
        QVERIFY(!links.first().error.isEmpty());
    }

    void warningsForWritesAndLongLife()
    {
        QVERIFY(PresignService::warnings("GET", 3600).isEmpty());
        QCOMPARE(PresignService::warnings("PUT", 3600).size(), 1);
        QCOMPARE(PresignService::warnings("GET", 3 * 24 * 3600).size(), 1);
        QCOMPARE(m_service->maxExpirySeconds(m_h->account.id), 7 * 24 * 3600);
    }

    // A GET link works without credentials; "download" names the file.
    void aLinkWorksWithoutCredentials()
    {
        PresignService::Request r;
        r.account = m_h->account.id;
        r.bucket = QStringLiteral("bkt");
        r.keys = {QStringLiteral("docs/report ä.pdf"), QStringLiteral("docs/b.txt")};
        r.expiresSeconds = 600;
        r.download = true;
        const QList<PresignService::Link> links = sign(r);
        QCOMPARE(links.size(), 2);
        QVERIFY(links[0].url.isValid());
        const QUrlQuery q(links[0].url);
        QCOMPARE(q.queryItemValue(QStringLiteral("X-Amz-Expires")), QStringLiteral("600"));
        QCOMPARE(q.queryItemValue(QStringLiteral("response-content-disposition"), QUrl::FullyDecoded),
                 QStringLiteral("attachment; filename=\"report _.pdf\""));
        QNetworkAccessManager nam;
        std::unique_ptr<QNetworkReply> reply(nam.get(QNetworkRequest(links[1].url)));
        QSignalSpy done(reply.get(), &QNetworkReply::finished);
        QVERIFY(done.wait(10000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(reply->readAll(), QByteArray("b"));
    }

    // Write access handed out is journaled — never the link itself.
    void putLinksAreJournaledWithoutTheUrl()
    {
        PresignService::Request r;
        r.account = m_h->account.id;
        r.bucket = QStringLiteral("bkt");
        r.keys = {QStringLiteral("incoming/upload.bin")};
        r.method = "PUT";
        const QList<PresignService::Link> links = sign(r);
        QVERIFY(links.first().url.isValid());
        m_journal->waitForIdle();
        QFile f(m_journal->fileFor(QDateTime::currentDateTimeUtc()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray journal = f.readAll();
        QVERIFY(journal.contains("PresignPut"));
        QVERIFY(journal.contains("incoming/upload.bin"));
        QVERIFY(!journal.contains("X-Amz-Signature"));
    }

    void theDialog()
    {
        PresignDialog dlg(m_service.get(), m_h->account.id, QStringLiteral("bkt"),
                          {QStringLiteral("docs/report ä.pdf"), QStringLiteral("docs/b.txt")}, 1);
        dlg.show();
        QVERIFY(dlg.generateButton()->isEnabled());
        QVERIFY(!dlg.copyButton()->isEnabled());
        dlg.amountBox()->setValue(8);
        dlg.unitBox()->setCurrentIndex(2);   // days: above the limit
        QCOMPARE(dlg.expiresSeconds(), 7 * 24 * 3600);
        QVERIFY(dlg.warningLabel()->text().contains(QLatin1String("limit")));
        dlg.amountBox()->setValue(2);
        dlg.unitBox()->setCurrentIndex(1);   // hours
        QSignalSpy ready(&dlg, &PresignDialog::linksReady);
        dlg.generateButton()->click();
        QVERIFY(ready.wait(10000));
        const QStringList lines = dlg.output()->toPlainText().split(u'\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[1].startsWith(QLatin1String("docs/b.txt\thttp")));
        QVERIFY(dlg.copyButton()->isEnabled());
        dlg.copyButton()->click();
        QCOMPARE(QGuiApplication::clipboard()->text(), dlg.output()->toPlainText());

        // PUT on a read-only account: not offered, with the reason.
        setReadOnly(true);
        dlg.methodBox()->setCurrentIndex(1);
        QVERIFY(!dlg.generateButton()->isEnabled());
        QVERIFY(!dlg.downloadBox()->isEnabled());
    }
};

QTEST_MAIN(TestPresign)
#include "test_presign.moc"
