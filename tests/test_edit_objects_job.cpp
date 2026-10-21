// EditObjectsJob through the OperationController against FakeObjectStore:
// a header / metadata / class edit keeps everything else of an object (tags
// the server drops are written back, the canned ACL is sent along), bulk
// edits of folders, every reason to leave an object alone, a server that
// ignores REPLACE, a change between plan and copy, unreadable permissions.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/EditObjectsJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;
using Change = EditObjectsJob::ChangeSpec;

namespace {
const QString kBucket = QStringLiteral("bkt");
} // namespace

class TestEditObjectsJob : public QObject {
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
    QList<Job::Preview> m_previews;
    std::function<void(Job*)> m_beforeConfirm;

    void useProvider(const QString& id)
    {
        Account a = m_h->account;
        a.providerId = id;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
    }

    EditObjectsJob* make(const QList<EditObjectsJob::Item>& items, const Change& change)
    {
        auto* job = new EditObjectsJob(m_h->account, kBucket, items, change, m_ops->planDir());
        job->setRecheckDelayForTesting(10);
        return job;
    }

    bool run(Job* job)
    {
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        return done.wait(30000) || done.size() == 1;
    }

    static QString reasons(const Job* job)
    {
        return (job->report().notes + job->report().failures).join(QStringLiteral(" | ")) + job->errorText();
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        useProvider(QStringLiteral("aws"));   // tag and ACL writes confirmed there
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
        m_previews.clear();
        m_beforeConfirm = nullptr;
        connect(m_ops.get(), &OperationController::previewNeeded, this, [this](Job* job) {
            m_previews.append(job->preview());
            if (m_beforeConfirm)
                m_beforeConfirm(job);
            job->confirm();
        });
        m_objects->putObject(kBucket, QStringLiteral("doc.txt"), "the content", QStringLiteral("text/plain"));
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

    void changeSpecs()
    {
        s3::ObjectHeaders before;
        before.contentType = QStringLiteral("text/plain");
        before.cacheControl = QStringLiteral("no-cache");
        before.userMetadata = {{QStringLiteral("a"), QStringLiteral("1")}, {QStringLiteral("b"), QStringLiteral("2")}};
        s3::ObjectHeaders after = before;
        after.cacheControl.clear();
        after.contentLanguage = QStringLiteral("de");
        after.userMetadata.remove(QStringLiteral("a"));
        after.userMetadata.insert(QStringLiteral("c"), QStringLiteral("3"));
        const Change c = Change::between(before, after);
        QVERIFY(!c.contentType);
        QCOMPARE(c.cacheControl, std::optional<QString>(QString()));   // removed
        QCOMPARE(c.contentLanguage, std::optional<QString>(QStringLiteral("de")));
        QCOMPARE(c.metadata.value(QStringLiteral("a")), std::optional<QString>());
        QVERIFY(c.metadata.contains(QStringLiteral("a")));
        QCOMPARE(c.metadata.value(QStringLiteral("c")), std::optional<QString>(QStringLiteral("3")));
        QVERIFY(!c.metadata.contains(QStringLiteral("b")));   // unchanged: left alone
        QCOMPARE(c.applyTo(before), after);
        // On another object the same change touches only what it names.
        s3::ObjectHeaders other;
        other.contentType = QStringLiteral("image/png");
        other.userMetadata = {{QStringLiteral("x"), QStringLiteral("9")}};
        const s3::ObjectHeaders merged = c.applyTo(other);
        QCOMPARE(merged.contentType, QStringLiteral("image/png"));
        QCOMPARE(merged.userMetadata.value(QStringLiteral("x")), QStringLiteral("9"));
        QCOMPARE(merged.userMetadata.value(QStringLiteral("c")), QStringLiteral("3"));
        QVERIFY(c.describe().join(u' ').contains(QLatin1String("Cache-Control: removed")));
        QVERIFY(Change().isEmpty());
    }

    // Tags (dropped by the copy, as RGW does), a public ACL and the class
    // survive an edit of the headers.
    void anEditKeepsEverythingElse()
    {
        const QString k = QStringLiteral("doc.txt");
        m_objects->setObjectHeader(kBucket, k, QStringLiteral("Cache-Control"), QStringLiteral("no-cache"));
        m_objects->setObjectTags(kBucket, k, {{QStringLiteral("project"), QStringLiteral("q")}});
        m_objects->setObjectAcl(kBucket, k, QStringLiteral("public-read"));
        m_objects->setObjectStorageClass(kBucket, k, QStringLiteral("STANDARD_IA"));
        m_objects->setDropTagsOnReplace(true);
        Change c;
        c.cacheControl = QStringLiteral("max-age=60");
        c.metadata.insert(QStringLiteral("stage"), QStringLiteral("edited"));
        auto* job = make({{k, false}}, c);
        QVERIFY(run(job));
        QVERIFY2(job->outcome() == Job::Outcome::Succeeded, qPrintable(reasons(job)));
        QVERIFY(job->report().verified);
        const auto o = m_objects->object(kBucket, k);
        QCOMPARE(o->data, QByteArray("the content"));
        QCOMPARE(o->contentType, QStringLiteral("text/plain"));
        QCOMPARE(o->headers.value(QStringLiteral("cache-control")), QStringLiteral("max-age=60"));
        QCOMPARE(o->metadata.value(QStringLiteral("stage")), QStringLiteral("edited"));
        QCOMPARE(o->tags.size(), 1);   // written back after the copy dropped them
        QCOMPARE(o->acl, QStringLiteral("public-read"));
        QCOMPARE(o->storageClass, QStringLiteral("STANDARD_IA"));
        QCOMPARE(m_objects->count(QStringLiteral("PutObjectTagging")), 1);
        QVERIFY(m_previews.first().details.join(u' ').contains(QLatin1String("Cache-Control: max-age=60")));
        QVERIFY(m_previews.first().permanent);   // no versions: the old headers are gone
    }

    void aFolderIsEditedObjectByObject()
    {
        for (const char* k : {"docs/", "docs/a.txt", "docs/b.txt", "docs/sub/c.txt"})
            m_objects->putObject(kBucket, QString::fromLatin1(k), k);
        Change c;
        c.contentLanguage = QStringLiteral("de");
        auto* job = make({{QStringLiteral("docs/"), true}}, c);
        QVERIFY(run(job));
        QVERIFY2(job->outcome() == Job::Outcome::Succeeded, qPrintable(reasons(job)));
        QCOMPARE(m_previews.first().objects, 3);   // the marker is left alone
        QCOMPARE(job->report().succeeded, 3);
        for (const char* k : {"docs/a.txt", "docs/b.txt", "docs/sub/c.txt"})
            QCOMPARE(m_objects->object(kBucket, QString::fromLatin1(k))->headers.value(QStringLiteral("content-language")),
                     QStringLiteral("de"));
        QVERIFY(m_objects->object(kBucket, QStringLiteral("docs/"))->headers.isEmpty());
    }

    void objectsThatCantBeCarriedOverAreLeftAlone_data()
    {
        QTest::addColumn<QString>("header");
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("reason");
        QTest::newRow("SSE-C") << QStringLiteral("x-amz-server-side-encryption-customer-algorithm") << QStringLiteral("AES256")
                               << QStringLiteral("customer key");
        QTest::newRow("KMS") << QStringLiteral("x-amz-server-side-encryption") << QStringLiteral("aws:kms")
                             << QStringLiteral("KMS");
        QTest::newRow("legal hold") << QStringLiteral("x-amz-object-lock-legal-hold") << QStringLiteral("ON")
                                    << QStringLiteral("legal hold");
        QTest::newRow("archive") << QString() << QStringLiteral("GLACIER") << QStringLiteral("archive");
    }

    void objectsThatCantBeCarriedOverAreLeftAlone()
    {
        QFETCH(QString, header);
        QFETCH(QString, value);
        QFETCH(QString, reason);
        const QString k = QStringLiteral("doc.txt");
        if (header.isEmpty())
            m_objects->setObjectStorageClass(kBucket, k, value);
        else
            m_objects->setObjectHeader(kBucket, k, header, value);
        Change c;
        c.cacheControl = QStringLiteral("max-age=1");
        auto* job = make({{k, false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->report().skipped, 1);
        QVERIFY2(job->report().notes.join(u' ').contains(reason), qPrintable(reasons(job)));
        QVERIFY(!m_objects->object(kBucket, k)->headers.contains(QStringLiteral("cache-control")));
        QCOMPARE(m_objects->count(QStringLiteral("CopyObject")), 0);
    }

    void retentionKeepsAnObjectOutOfAnEdit()
    {
        const QString k = QStringLiteral("doc.txt");
        m_objects->setObjectHeader(kBucket, k, QStringLiteral("x-amz-object-lock-mode"), QStringLiteral("GOVERNANCE"));
        m_objects->setObjectHeader(kBucket, k, QStringLiteral("x-amz-object-lock-retain-until-date"),
                                   QDateTime::currentDateTimeUtc().addDays(30).toString(Qt::ISODateWithMs));
        Change c;
        c.cacheControl = QStringLiteral("max-age=1");
        auto* job = make({{k, false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->report().skipped, 1);
        QVERIFY(job->report().notes.join(u' ').contains(QLatin1String("retention")));
    }

    // Writing tags / ACLs not confirmed (a generic server): objects with tags
    // or a public ACL are left alone, the others are edited.
    void unconfirmedWritesLeaveTaggedAndPublicObjectsAlone()
    {
        useProvider(QStringLiteral("generic"));
        m_objects->putObject(kBucket, QStringLiteral("tagged.txt"), "t");
        m_objects->setObjectTags(kBucket, QStringLiteral("tagged.txt"), {{QStringLiteral("k"), QStringLiteral("v")}});
        m_objects->putObject(kBucket, QStringLiteral("public.txt"), "p");
        m_objects->setObjectAcl(kBucket, QStringLiteral("public.txt"), QStringLiteral("public-read"));
        Change c;
        c.cacheControl = QStringLiteral("max-age=5");
        auto* job = make({{QStringLiteral("doc.txt"), false}, {QStringLiteral("tagged.txt"), false},
                          {QStringLiteral("public.txt"), false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->report().succeeded, 1);
        QCOMPARE(job->report().skipped, 2);
        QVERIFY(job->report().notes.join(u' ').contains(QLatin1String("tags")));
        QVERIFY(job->report().notes.join(u' ').contains(QLatin1String("public-read")));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("public.txt"))->acl, QStringLiteral("public-read"));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("tagged.txt"))->tags.size(), 1);
        QVERIFY(m_previews.first().warnings.join(u' ').contains(QLatin1String("tags")));
    }

    // VersityGW-like: the copy "succeeds" but keeps the old headers — the
    // check says so instead of reporting success.
    void aServerThatKeepsOldHeadersIsCaught()
    {
        m_objects->setObjectHeader(kBucket, QStringLiteral("doc.txt"), QStringLiteral("Cache-Control"), QStringLiteral("old"));
        m_objects->setIgnoreMetadataReplace(true);
        Change c;
        c.cacheControl = QStringLiteral("new");
        auto* job = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY2(job->report().failures.first().contains(QLatin1String("Cache-Control")), qPrintable(reasons(job)));
        QVERIFY(!job->report().verified);
    }

    void anObjectChangedAfterThePlanIsLeftAlone()
    {
        m_beforeConfirm = [this](Job*) { m_objects->putObject(kBucket, QStringLiteral("doc.txt"), "rewritten"); };
        Change c;
        c.cacheControl = QStringLiteral("max-age=1");
        auto* job = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->report().skipped, 1);
        QVERIFY(job->report().notes.first().contains(QLatin1String("changed since it was planned")));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("doc.txt"))->data, QByteArray("rewritten"));
    }

    void theStorageClassChanges()
    {
        Change c;
        c.storageClass = QStringLiteral("STANDARD_IA");
        auto* job = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(job));
        QVERIFY2(job->outcome() == Job::Outcome::Succeeded, qPrintable(reasons(job)));
        QCOMPARE(m_objects->object(kBucket, QStringLiteral("doc.txt"))->storageClass, QStringLiteral("STANDARD_IA"));
        QCOMPARE(m_previews.first().action, QStringLiteral("Change Class"));
        // An archive class as target is said so.
        c.storageClass = QStringLiteral("DEEP_ARCHIVE");
        m_beforeConfirm = [](Job* j) { j->cancel(); };
        auto* archive = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(archive));
        QVERIFY(m_previews.last().warnings.join(u' ').contains(QLatin1String("archive class")));
    }

    // Permissions that can't be read: an edit could make public objects
    // private — that object is not changed. Not permitted at all: the job
    // is refused before anything is sent.
    void unreadablePermissionsRefuseTheEdit()
    {
        m_h->server->addRoute({"GET", {}, {"acl"}, {}}, {FakeResponse::s3Error(500, "InternalError")});
        Change c;
        c.cacheControl = QStringLiteral("max-age=1");
        auto* job = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY2(job->report().failures.join(u' ').contains(QLatin1String("could not be read")), qPrintable(reasons(job)));
        QCOMPARE(m_objects->count(QStringLiteral("CopyObject")), 0);
    }

    void permissionsNotPermittedRefuseTheJob()
    {
        m_h->server->addRoute({"GET", {}, {"acl"}, {}}, {FakeResponse::s3Error(403, "AccessDenied")});
        Change c;
        c.cacheControl = QStringLiteral("max-age=1");
        auto* job = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(job));
        QCOMPARE(job->outcome(), Job::Outcome::Failed);
        QVERIFY2(job->errorText().contains(QLatin1String("permissions")), qPrintable(job->errorText()));
        QCOMPARE(m_objects->count(QStringLiteral("CopyObject")), 0);
    }

    // A server without object tags (501 on ?tagging): nothing to lose, edited.
    void noTagsOnTheServerIsNoObstacle()
    {
        m_h->server->addRoute({"GET", {}, {"tagging"}, {}}, {FakeResponse::s3Error(501, "NotImplemented")});
        Change c;
        c.cacheControl = QStringLiteral("max-age=1");
        auto* job = make({{QStringLiteral("doc.txt"), false}}, c);
        QVERIFY(run(job));
        QVERIFY2(job->outcome() == Job::Outcome::Succeeded, qPrintable(reasons(job)));
    }
};

QTEST_GUILESS_MAIN(TestEditObjectsJob)
#include "test_edit_objects_job.moc"
