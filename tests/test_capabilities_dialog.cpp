#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "support/AccountHarness.h"
#include "ui/CapabilitiesDialog.h"
#include "ui/LogPanel.h"
#include "ui/ThemeManager.h"

#include <QComboBox>
#include <QDir>
#include <QPushButton>
#include <QTreeWidget>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QByteArray kBuckets = "<ListAllMyBucketsResult><Buckets>"
                            "<Bucket><Name>bkt</Name><CreationDate>2026-01-01T00:00:00Z</CreationDate></Bucket>"
                            "</Buckets></ListAllMyBucketsResult>";

QTreeWidgetItem* row(QTreeWidget* table, const QString& feature)
{
    const auto found = table->findItems(feature, Qt::MatchExactly, 0);
    return found.isEmpty() ? nullptr : found.first();
}

} // namespace

class TestCapabilitiesDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_theme.apply(QStringLiteral("dark"));
    }

    void init()
    {
        m_h = std::make_unique<AccountHarness>(QStringLiteral("Lab"));
        QVERIFY(m_h->ok());
        m_website = FakeResponse::s3Error(501, "NotImplemented");
        m_h->server->setFallbackHandler([this](const RecordedRequest& r) {
            if (r.path == "/")
                return FakeResponse::ok(kBuckets).withHeader("Server", "FakeGW/2.0");
            if (r.hasQueryItem("versioning"))
                return FakeResponse::ok("<VersioningConfiguration><Status>Suspended</Status></VersioningConfiguration>");
            if (r.hasQueryItem("website"))
                return m_website;
            if (r.hasQueryItem("list-type"))
                return FakeResponse::ok("<ListBucketResult><Name>bkt</Name><IsTruncated>false</IsTruncated></ListBucketResult>");
            if (!r.query.isEmpty())
                return FakeResponse::s3Error(501, "NotImplemented");
            return FakeResponse();
        });
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->serverSet(m_h->account.id).probedAt.isValid(), 10000);
        m_caps->bucketOpened(m_h->account.id, QStringLiteral("bkt"));
        QTRY_VERIFY_WITH_TIMEOUT(m_caps->bucketSet(m_h->account.id, QStringLiteral("bkt")).has_value(), 10000);
    }

    void cleanup()
    {
        m_caps.reset();
        m_log.reset();
        m_store.reset();
        m_h.reset();
    }

    void showsEveryFeatureWithItsState()
    {
        CapabilitiesDialog dlg(m_caps.get(), m_h->account.id, QStringLiteral("Lab"), QStringLiteral("bkt"));
        dlg.show();
        QCOMPARE(dlg.scope(), QStringLiteral("bkt"));
        auto* table = dlg.findChild<QTreeWidget*>(QStringLiteral("capabilityTable"));
        QCOMPARE(table->topLevelItemCount(), int(s3::allFeatures().size()));
        QTreeWidgetItem* website = row(table, CapabilityController::featureName(s3::Feature::Website));
        QVERIFY(website);
        QCOMPARE(website->text(1), CapabilityController::stateText(s3::CapabilityState::Unsupported));
        QVERIFY(website->text(4).contains(QLatin1String("501")));
        QVERIFY(website->toolTip(0).contains(QLatin1String("501")));

        dlg.setScope(QString());   // the server
        QCOMPARE(table->topLevelItemCount(), int(s3::allFeatures().size()));
        auto* scopes = dlg.findChild<QComboBox*>();
        QCOMPARE(scopes->count(), 2);   // server + bkt
    }

    void checkAgainRecordsAChange()
    {
        CapabilitiesDialog dlg(m_caps.get(), m_h->account.id, QStringLiteral("Lab"), QStringLiteral("bkt"));
        LogPanel log(m_log.get());
        dlg.show();
        auto* history = dlg.findChild<QTreeWidget*>(QStringLiteral("capabilityHistory"));
        QTRY_COMPARE(history->topLevelItemCount(), 1);   // "No changes recorded."

        m_website = FakeResponse::ok("<WebsiteConfiguration><IndexDocument><Suffix>index.html</Suffix>"
                                     "</IndexDocument></WebsiteConfiguration>");
        QPushButton* again = nullptr;
        for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == CapabilitiesDialog::tr("Check Again"))
                again = b;
        }
        QVERIFY(again);
        again->click();
        QVERIFY(!again->isEnabled());   // "Checking…"
        QTRY_VERIFY_WITH_TIMEOUT(again->isEnabled(), 10000);
        auto* table = dlg.findChild<QTreeWidget*>(QStringLiteral("capabilityTable"));
        QCOMPARE(row(table, CapabilityController::featureName(s3::Feature::Website))->text(1),
                 CapabilityController::stateText(s3::CapabilityState::Supported));
        QTRY_VERIFY(history->topLevelItem(0)->text(2) == CapabilityController::featureName(s3::Feature::Website));
        QVERIFY(log.toPlainText().contains(CapabilityController::featureName(s3::Feature::Website)));

        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            QVERIFY(dlg.grab().save(QDir(dir).filePath(QStringLiteral("capabilities-dark.png"))));
        }
    }

private:
    ThemeManager m_theme;
    std::unique_ptr<AccountHarness> m_h;
    FakeResponse m_website;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
};

QTEST_MAIN(TestCapabilitiesDialog)
#include "test_capabilities_dialog.moc"
