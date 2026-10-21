// The configuration pages of BucketPropertiesDialog: what is set is read
// fresh and shown (table, form, XML / JSON), edits are applied only through
// the OperationController, forms never drop what they can't show, pages say
// why they are read-only, and unsaved edits are noticed.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/Job.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "support/AccountHarness.h"
#include "ui/AclSection.h"
#include "ui/BucketPropertiesDialog.h"
#include "ui/CodeEditor.h"
#include "ui/CorsPage.h"
#include "ui/LifecyclePage.h"
#include "ui/ObjectLockPage.h"
#include "ui/OtherConfigPage.h"
#include "ui/PolicyPage.h"
#include "ui/PublicAccessBlockSection.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/BucketConfig.h>
#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QCheckBox>
#include <QDir>
#include <QTranslator>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");

QByteArray cors(int rules)
{
    s3::CorsConfiguration c;
    for (int i = 0; i < rules; ++i) {
        s3::CorsRule r;
        r.id = QStringLiteral("r%1").arg(i);
        r.allowedMethods = {QStringLiteral("GET")};
        r.allowedOrigins = {QStringLiteral("https://%1.example").arg(i)};
        c.rules.append(r);
    }
    return s3::buildCorsConfiguration(c).value();
}

} // namespace

class TestBucketConfigPages : public QObject {
    Q_OBJECT

private:
    ThemeManager m_theme;
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<OperationController> m_ops;
    int m_previews = 0;

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    void probe()
    {
        QVERIFY(m_h->sessions->session(m_h->account.id));
        QTRY_VERIFY(m_caps->isLoaded(m_h->account.id));
        m_caps->bucketOpened(m_h->account.id, kBucket);
        QTRY_VERIFY_WITH_TIMEOUT(!m_caps->isProbing(m_h->account.id, kBucket) && m_caps->bucketSet(m_h->account.id, kBucket),
                                 10000);
    }

    std::unique_ptr<BucketPropertiesDialog> dialog()
    {
        auto dlg = std::make_unique<BucketPropertiesDialog>(m_h->account.id, kBucket, m_h->sessions.get(), m_ops.get(),
                                                            m_caps.get());
        dlg->setAskBeforeDiscarding(false);
        return dlg;
    }

    static bool loadedPage(BucketConfigPage* page)
    {
        page->ensureLoaded();
        return QTest::qWaitFor([page] { return page->isLoaded(); }, 10000);
    }

    // Clicks Apply and waits until the page has read the result back.
    static bool apply(BucketConfigPage* page)
    {
        if (!page->applyButton()->isEnabled())
            return false;
        QSignalSpy loaded(page, &BucketConfigPage::loaded);
        page->applyButton()->click();
        return loaded.wait(20000) && !page->isRunning();
    }

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
        m_objects->putObject(kBucket, QStringLiteral("a.txt"), "a");
        m_store = std::make_unique<CapabilityStore>(m_h->path(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_h->accounts.get(), m_h->sessions.get(), m_store.get(),
                                                        m_log.get());
        m_journal = std::make_unique<Journal>(m_h->path(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_h->accounts.get(), m_h->sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_ops->setPlanDir(m_h->path(QStringLiteral("plans")));
        m_previews = 0;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            ++m_previews;
            job->confirm();
        });
        useProvider(QStringLiteral("aws"));
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

    void corsTableWritesTheXml()
    {
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::Cors, cors(2));
        probe();
        auto dlg = dialog();
        CorsPage* page = dlg->corsPage();
        QVERIFY(loadedPage(page));
        QCOMPARE(page->stateLabel()->text(), QStringLiteral("Set"));
        QCOMPARE(page->table()->rowCount(), 2);
        QCOMPARE(page->table()->item(1, CorsPage::OriginsColumn)->text(), QStringLiteral("https://1.example"));
        QVERIFY(!page->isDirty());
        QVERIFY(!page->applyButton()->isEnabled());

        page->addButton()->click();
        QCOMPARE(page->table()->rowCount(), 3);
        QVERIFY(page->isDirty());
        QVERIFY(page->xmlEditor()->toPlainText().contains(QLatin1String("https://example.com")));
        QVERIFY(dlg->hasUnsavedChanges());
        // An invalid method: shown, not applicable.
        page->table()->item(2, CorsPage::MethodsColumn)->setText(QStringLiteral("PATCH"));
        QVERIFY(!page->applyButton()->isEnabled());
        QVERIFY(page->problemLabel()->isVisibleTo(page));
        page->table()->item(2, CorsPage::MethodsColumn)->setText(QStringLiteral("get, head"));
        QVERIFY(apply(page));
        QCOMPARE(m_previews, 1);
        const s3::Result<s3::CorsConfiguration> stored =
            s3::parseCorsConfiguration(*m_objects->bucketConfig(kBucket, s3::BucketConfig::Cors));
        QVERIFY(stored);
        QCOMPARE(stored->rules.size(), 3);
        QCOMPARE(stored->rules[2].allowedMethods, (QStringList{QStringLiteral("GET"), QStringLiteral("HEAD")}));
        QVERIFY(!page->isDirty());
        QVERIFY(!dlg->hasUnsavedChanges());

        QVERIFY(page->removeButton()->isEnabled());
        QSignalSpy loaded(page, &BucketConfigPage::loaded);
        page->removeButton()->click();
        QVERIFY(loaded.wait(20000));
        QVERIFY(!m_objects->bucketConfig(kBucket, s3::BucketConfig::Cors).has_value());
        QCOMPARE(page->stateLabel()->text(), QStringLiteral("Not set"));
        QCOMPARE(page->table()->rowCount(), 0);
    }

    void revertBringsBackWhatIsSet()
    {
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::Cors, cors(1));
        probe();
        auto dlg = dialog();
        CorsPage* page = dlg->corsPage();
        QVERIFY(loadedPage(page));
        page->table()->removeRow(0);
        page->removeRuleButton()->click();   // nothing selected: nothing happens
        page->xmlEditor()->setPlainText(QString());
        QVERIFY(page->isDirty());
        page->revertButton()->click();
        QVERIFY(!page->isDirty());
        QCOMPARE(page->table()->rowCount(), 1);
    }

    // A rule element the form can't show: XML only, and applying the XML
    // keeps it.
    void lifecycleTheFormCantShowIsXmlOnly()
    {
        const QByteArray unknown =
            "<LifecycleConfiguration><Rule><ID>big</ID><Filter><And><Prefix>media/</Prefix>"
            "<ObjectSizeGreaterThan>1048576</ObjectSizeGreaterThan></And></Filter><Status>Enabled</Status>"
            "<Expiration><Days>30</Days></Expiration></Rule></LifecycleConfiguration>";
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::Lifecycle, unknown);
        probe();
        auto dlg = dialog();
        LifecyclePage* page = dlg->lifecyclePage();
        QVERIFY(loadedPage(page));
        QCOMPARE(page->tabs()->currentIndex(), 1);
        QVERIFY(!page->addButton()->isEnabled());
        QVERIFY(!page->prefixEdit()->isEnabled());
        QVERIFY(!page->xmlEditor()->isReadOnly());
        QString xml = page->xmlEditor()->toPlainText();
        xml.replace(QStringLiteral("<Days>30</Days>"), QStringLiteral("<Days>60</Days>"));
        page->xmlEditor()->setPlainText(xml);
        QVERIFY(apply(page));
        const QByteArray stored = *m_objects->bucketConfig(kBucket, s3::BucketConfig::Lifecycle);
        QVERIFY(stored.contains("ObjectSizeGreaterThan"));
        QVERIFY(stored.contains("<Days>60</Days>"));
    }

    void lifecycleForm()
    {
        probe();
        auto dlg = dialog();
        LifecyclePage* page = dlg->lifecyclePage();
        QVERIFY(loadedPage(page));
        QCOMPARE(page->stateLabel()->text(), QStringLiteral("Not set"));
        QVERIFY(!page->transitionRow()->isHidden());   // AWS knows more than one storage class
        page->addButton()->click();
        QCOMPARE(page->rules()->count(), 1);
        page->prefixEdit()->setText(QStringLiteral("tmp/"));
        QTest::keyClick(page->prefixEdit(), Qt::Key_End);   // textEdited: typed, not set
        QTest::keyClicks(page->prefixEdit(), QStringLiteral("x"));
        page->expireDays()->setValue(3);
        page->noncurrentCheck()->setChecked(true);
        page->noncurrentDays()->setValue(5);
        page->abortCheck()->setChecked(true);
        QVERIFY(apply(page));
        const s3::Result<s3::LifecycleConfiguration> stored =
            s3::parseLifecycleConfiguration(*m_objects->bucketConfig(kBucket, s3::BucketConfig::Lifecycle));
        QVERIFY(stored);
        QCOMPARE(stored->rules.size(), 1);
        const s3::LifecycleRule& rule = stored->rules.first();
        QCOMPARE(rule.prefix, QStringLiteral("tmp/x"));
        QCOMPARE(rule.expirationDays, std::optional(3));
        QCOMPARE(rule.noncurrentDays, std::optional(5));
        QVERIFY(rule.abortIncompleteDays.has_value());
    }

    void policyTemplatesAndFindings()
    {
        probe();
        auto dlg = dialog();
        PolicyPage* page = dlg->policyPage();
        QVERIFY(loadedPage(page));
        QVERIFY(page->editor()->toPlainText().isEmpty());
        QVERIFY(!page->removeButton()->isEnabled());   // nothing to remove
        page->templates()->setCurrentIndex(1);
        emit page->templates()->activated(1);
        QVERIFY(page->editor()->toPlainText().contains(QLatin1String("arn:aws:s3:::bkt/*")));
        QCOMPARE(page->templates()->currentIndex(), 0);
        QVERIFY(page->findings()->isVisibleTo(page));
        QVERIFY(page->applyButton()->isEnabled());
        page->editor()->setPlainText(QStringLiteral("{ not json"));
        QVERIFY(!page->applyButton()->isEnabled());
        QVERIFY(page->problemLabel()->text().contains(QLatin1String("not JSON")));
        emit page->templates()->activated(2);
        QVERIFY(apply(page));
        QVERIFY(m_objects->bucketConfig(kBucket, s3::BucketConfig::Policy)->contains("aws:SecureTransport"));
    }

    void unconfirmedWritesAreReadOnlyWithTheReason()
    {
        useProvider(QStringLiteral("generic"));
        probe();
        auto dlg = dialog();
        PolicyPage* page = dlg->policyPage();
        QVERIFY(loadedPage(page));
        QVERIFY(page->editor()->isReadOnly());
        QVERIFY(!page->templates()->isEnabled());
        QVERIFY(page->readOnlyReason().contains(QLatin1String("not confirmed")));
    }

    void unsupportedKindSaysSo()
    {
        m_objects->setUnsupportedBucketConfig(s3::BucketConfig::Cors);
        probe();
        auto dlg = dialog();
        CorsPage* page = dlg->corsPage();
        page->ensureLoaded();
        QTRY_VERIFY_WITH_TIMEOUT(page->stateLabel()->text().contains(QLatin1String("Not supported")), 10000);
        QVERIFY(!page->isEditable());
    }

    void accessPage()
    {
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::Policy,
                                   "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Principal\":\"*\","
                                   "\"Action\":\"s3:GetObject\",\"Resource\":\"arn:aws:s3:::bkt/*\"}]}");
        probe();
        auto dlg = dialog();
        AclSection* acl = dlg->aclSection();
        QVERIFY(loadedPage(acl));
        QCOMPARE(acl->cannedBox()->currentData().toString(), QStringLiteral("private"));
        QCOMPARE(acl->grants()->topLevelItemCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(dlg->policyStatusLabel()->text(), QStringLiteral("yes"), 10000);
        acl->cannedBox()->setCurrentIndex(acl->cannedBox()->findData(QStringLiteral("public-read")));
        QVERIFY(apply(acl));
        QCOMPARE(*m_objects->bucketConfig(kBucket, s3::BucketConfig::Acl), QByteArray("public-read"));
        QCOMPARE(acl->grants()->topLevelItemCount(), 2);

        PublicAccessBlockSection* pab = dlg->publicAccessBlockSection();
        QVERIFY(loadedPage(pab));
        QVERIFY(!pab->blockAcls()->isChecked());
        pab->blockAcls()->setChecked(true);
        pab->blockPolicy()->setChecked(true);
        QVERIFY(apply(pab));
        const s3::Result<s3::PublicAccessBlockConfiguration> stored =
            s3::parsePublicAccessBlock(*m_objects->bucketConfig(kBucket, s3::BucketConfig::PublicAccessBlock));
        QVERIFY(stored && stored->blockPublicAcls && stored->blockPublicPolicy && !stored->ignorePublicAcls);
    }

    void objectLockPage()
    {
        probe();
        {
            auto dlg = dialog();
            ObjectLockPage* page = dlg->objectLockPage();
            page->ensureLoaded();
            QTRY_VERIFY_WITH_TIMEOUT(page->isLoaded(), 10000);
            QVERIFY(!page->defaultCheck()->isEnabled());   // no object lock on this bucket
        }
        m_objects->setObjectLockEnabled(kBucket);
        m_caps->recheck(m_h->account.id, kBucket);
        QTRY_VERIFY_WITH_TIMEOUT(!m_caps->isProbing(m_h->account.id, kBucket)
                                     && m_caps->versioningOf(m_h->account.id, kBucket).objectLock.value_or(false),
                                 10000);
        auto dlg = dialog();
        ObjectLockPage* page = dlg->objectLockPage();
        QVERIFY(loadedPage(page));
        QVERIFY(page->defaultCheck()->isEnabled());
        page->defaultCheck()->setChecked(true);
        page->periodBox()->setValue(7);
        QVERIFY(apply(page));
        const s3::Result<s3::ObjectLockConfiguration> stored =
            s3::parseObjectLockConfiguration(*m_objects->bucketConfig(kBucket, s3::BucketConfig::ObjectLock));
        QVERIFY(stored);
        QCOMPARE(stored->mode, std::optional(s3::ObjectLockMode::Governance));
        QCOMPARE(stored->days, std::optional(7));
        page->defaultCheck()->setChecked(false);   // removing it is a write too
        QVERIFY(apply(page));
        QVERIFY(!s3::parseObjectLockConfiguration(*m_objects->bucketConfig(kBucket, s3::BucketConfig::ObjectLock))->mode);
    }

    void otherSettingsAreShownOnly()
    {
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::Tagging,
                                   "<Tagging><TagSet><Tag><Key>team</Key><Value>web</Value></Tag></TagSet></Tagging>");
        probe();
        auto dlg = dialog();
        OtherConfigPage* page = dlg->otherPage();
        QSignalSpy loaded(page, &OtherConfigPage::loaded);
        page->ensureLoaded();
        QVERIFY(loaded.wait(10000));
        QTreeWidgetItem* tags = page->list()->topLevelItem(0);
        QCOMPARE(tags->text(1), QStringLiteral("Set"));
        QCOMPARE(page->list()->topLevelItem(1)->text(1), QStringLiteral("Not set"));
        page->list()->setCurrentItem(tags);
        QVERIFY(page->viewer()->toPlainText().contains(QLatin1String("<Key>team</Key>")));
        QVERIFY(page->viewer()->isReadOnly());
    }

    // QBUCKET_SCREENSHOT_DIR (with a real platform, or offscreen to look):
    // bucket-policy.png, bucket-lifecycle.png, bucket-object-lock.png,
    // bucket-access.png.
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
        m_objects->setObjectLockEnabled(kBucket);
        s3::LifecycleConfiguration lc;
        s3::LifecycleRule logs;
        logs.id = QStringLiteral("expire-logs");
        logs.prefix = QStringLiteral("logs/");
        logs.expirationDays = 90;
        logs.noncurrentDays = 30;
        logs.newerNoncurrentVersions = 2;
        s3::LifecycleRule uploads;
        uploads.id = QStringLiteral("abort-uploads");
        uploads.abortIncompleteDays = 7;
        lc.rules = {logs, uploads};
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::Lifecycle, s3::buildLifecycleConfiguration(lc).value());
        s3::ObjectLockConfiguration lock;
        lock.enabled = true;
        lock.mode = s3::ObjectLockMode::Governance;
        lock.days = 30;
        m_objects->setBucketConfig(kBucket, s3::BucketConfig::ObjectLock, s3::buildObjectLockConfiguration(lock));
        probe();
        auto dlg = dialog();
        dlg->resize(900, 600);
        dlg->show();
        const auto shoot = [&](BucketPropertiesDialog::Page page, BucketConfigPage* config, const char* file) {
            dlg->showPage(page);
            if (config)
                QVERIFY(loadedPage(config));
            QTest::qWait(300);
            QVERIFY(dlg->grab().save(QDir(dir).filePath(QLatin1String(file))));
        };
        shoot(BucketPropertiesDialog::PolicySettingsPage, dlg->policyPage(), "bucket-policy.png");
        dlg->policyPage()->templates()->setCurrentIndex(1);
        emit dlg->policyPage()->templates()->activated(1);
        shoot(BucketPropertiesDialog::PolicySettingsPage, nullptr, "bucket-policy.png");
        shoot(BucketPropertiesDialog::LifecycleSettingsPage, dlg->lifecyclePage(), "bucket-lifecycle.png");
        dlg->lifecyclePage()->rules()->setCurrentRow(0);
        shoot(BucketPropertiesDialog::LifecycleSettingsPage, nullptr, "bucket-lifecycle.png");
        shoot(BucketPropertiesDialog::ObjectLockSettingsPage, dlg->objectLockPage(), "bucket-object-lock.png");
        shoot(BucketPropertiesDialog::AccessSettingsPage, dlg->aclSection(), "bucket-access.png");
        m_theme.apply(QStringLiteral("Light"));
    }
};

QTEST_MAIN(TestBucketConfigPages)
#include "test_bucket_config_pages.moc"
