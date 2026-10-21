// The app's mutation jobs against a real S3 server — opt-in, like
// libs/qbucket-s3/tests/it_s3_backend: runs when QBUCKET_IT_ENDPOINT is set
// (QBUCKET_IT_REGION, _ACCESS_KEY, _SECRET_KEY, _BUCKET, _ADDRESSING,
// _PROVIDER, _BACKEND; optional _CREATE_BUCKET=1 for local backends).
//
// Safety: refuses any bucket not named qbucket-it-*; every key lives under
// it/<run id>/ and only that is cleaned up. The account is "Development",
// so no production confirmations are involved — the gate still runs.

#include "accounts/CapabilityStore.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CopyJob.h"
#include "controllers/CreateFolderJob.h"
#include "controllers/DeleteJob.h"
#include "controllers/DownloadJob.h"
#include "controllers/EditObjectsJob.h"
#include "controllers/ObjectPropertyJob.h"
#include "controllers/RenameJob.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"
#include "support/FakeKeychainBackend.h"

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"

#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

QString env(const char* name)
{
    return qEnvironmentVariable(name);
}

} // namespace

class ItOperations : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (env("QBUCKET_IT_ENDPOINT").isEmpty())
            QSKIP("QBUCKET_IT_ENDPOINT not set — integration tests are opt-in");
        m_bucket = env("QBUCKET_IT_BUCKET");
        static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
        QVERIFY2(safeName.match(m_bucket).hasMatch(),
                 "refusing to run: QBUCKET_IT_BUCKET must be a dedicated qbucket-it-* bucket");
        QVERIFY(m_dir.isValid());
        m_root = QStringLiteral("it/%1/").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));

        m_accounts = std::make_unique<AccountStore>(m_dir.filePath(QStringLiteral("accounts.ini")));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("it-%1").arg(env("QBUCKET_IT_BACKEND"));
        a.providerId = env("QBUCKET_IT_PROVIDER").isEmpty() ? QStringLiteral("generic") : env("QBUCKET_IT_PROVIDER");
        a.region = env("QBUCKET_IT_REGION");
        a.endpoint = env("QBUCKET_IT_ENDPOINT");
        a.addressing = env("QBUCKET_IT_ADDRESSING") == QLatin1String("virtual") ? s3::AddressingStyle::VirtualHosted
                                                                                : s3::AddressingStyle::Path;
        a.accessKeyId = env("QBUCKET_IT_ACCESS_KEY");
        a.environment = Environment::Development;
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                env("QBUCKET_IT_SECRET_KEY"));
        m_secrets = std::make_unique<SecretStore>(std::move(backend));
        QVERIFY(m_accounts->save(a));
        m_account = a;
        m_sessions = std::make_unique<AccountSessions>(m_accounts.get(), m_secrets.get());
        m_store = std::make_unique<CapabilityStore>(m_dir.filePath(QStringLiteral("caps")));
        m_log = std::make_unique<ActivityLog>();
        m_caps = std::make_unique<CapabilityController>(m_accounts.get(), m_sessions.get(), m_store.get(), m_log.get());
        m_journal = std::make_unique<Journal>(m_dir.filePath(QStringLiteral("journal")));
        m_ops = std::make_unique<OperationController>(m_accounts.get(), m_sessions.get(), m_caps.get(),
                                                      m_journal.get(), m_log.get());
        m_transfers = std::make_unique<TransferService>(m_dir.filePath(QStringLiteral("transfers")), nullptr);
        m_ops->setTransfers(m_transfers.get());
        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));

        if (env("QBUCKET_IT_CREATE_BUCKET") == QLatin1String("1")) {
            s3::CreateBucketParams params;
            params.bucket = m_bucket;
            params.region = a.region;
            QObject ctx;
            bool done = false;
            session()->createBucket(params, &ctx, [&](const s3::Result<void>&) { done = true; });   // exists: fine
            QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        }
    }

    void cleanupTestCase()
    {
        if (!m_sessions)
            return;
        // Everything below this run's root, and nothing else.
        QStringList keys;
        QString token;
        for (int page = 0; page < 100; ++page) {
            s3::ListObjectsParams p;
            p.bucket = m_bucket;
            p.prefix = m_root;
            p.continuationToken = token;
            std::optional<s3::Result<s3::ListPage>> r;
            QObject ctx;
            session()->listObjects(p, &ctx, [&](s3::Result<s3::ListPage> res) { r.emplace(std::move(res)); });
            QTRY_VERIFY_WITH_TIMEOUT(r.has_value(), 30000);
            if (!*r)
                break;
            for (const s3::ObjectEntry& e : (*r)->objects)
                keys.append(e.key);
            if (!(*r)->isTruncated)
                break;
            token = (*r)->nextContinuationToken;
        }
        for (const QString& k : std::as_const(keys)) {
            QVERIFY(k.startsWith(m_root));
            QObject ctx;
            bool done = false;
            session()->deleteObject(m_bucket, k, QString(), &ctx, [&](const s3::Result<s3::DeleteObjectResult>&) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        }
        // Unfinished multipart uploads of this run (a failed upload job).
        s3::ListMultipartUploadsParams up;
        up.bucket = m_bucket;
        up.prefix = m_root;
        std::optional<s3::Result<s3::ListMultipartUploadsPage>> open;
        QObject listCtx;
        session()->listMultipartUploads(up, &listCtx, [&](s3::Result<s3::ListMultipartUploadsPage> r) { open.emplace(std::move(r)); });
        QTRY_VERIFY_WITH_TIMEOUT(open.has_value(), 30000);
        if (*open) {
            for (const s3::MultipartUploadInfo& u : (*open)->uploads) {
                QVERIFY(u.key.startsWith(m_root));
                QObject ctx;
                bool done = false;
                session()->abortMultipartUpload(m_bucket, u.key, u.uploadId, &ctx, [&](const s3::Result<void>&) { done = true; });
                QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
            }
        }
        m_ops.reset();
        m_transfers.reset();
        m_journal.reset();
        m_caps.reset();
        m_sessions.reset();
        m_secrets.reset();
    }

    void createFolder()
    {
        Job* job = run(new CreateFolderJob(m_account, m_bucket, m_root, QStringLiteral("new folder ä")));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QCOMPARE(job->report().succeeded, 1);

        // Again: it exists now, nothing is written.
        job = run(new CreateFolderJob(m_account, m_bucket, m_root, QStringLiteral("new folder ä")));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().skipped, 1);
    }

    void deleteAFolderRecursively()
    {
        const QString folder = m_root + QStringLiteral("doomed/");
        const QStringList keys = {folder + QStringLiteral("a.txt"), folder + QStringLiteral("sub/b c.txt"),
                                  folder + QStringLiteral("sub/deeper/ü+%.bin"), folder};
        for (const QString& key : keys)
            QVERIFY(put(key, key.endsWith(u'/') ? QByteArray() : key.toUtf8()));
        const QString keep = m_root + QStringLiteral("doomed-not.txt");   // shares the prefix text
        QVERIFY(put(keep, "keep"));

        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));
        auto* job = new DeleteJob(m_account, m_bucket, {{folder, true}}, m_ops->planDir());
        job->setRecheckDelaysForTesting(2000, 5000);
        run(job);
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, keys.size());
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QVERIFY(exists(keep));
        for (const QString& key : keys)
            QVERIFY2(!exists(key), qPrintable(key));
    }

    void renameAnObjectAndAFolder()
    {
        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));
        const QString file = m_root + QStringLiteral("before ä.txt");
        QVERIFY(put(file, "rename me"));
        Job* job = run(new RenameJob(m_account, m_bucket, file, false, QStringLiteral("after ü.txt"), m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        QVERIFY(!exists(file));
        QVERIFY(exists(m_root + QStringLiteral("after ü.txt")));

        const QString folder = m_root + QStringLiteral("dir old/");
        const QStringList keys = {folder, folder + QStringLiteral("a"), folder + QStringLiteral("sub/b+c")};
        for (const QString& k : keys)
            QVERIFY(put(k, k.endsWith(u'/') ? QByteArray() : k.toUtf8()));
        job = run(new RenameJob(m_account, m_bucket, folder, true, QStringLiteral("dir new"), m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, keys.size());
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        for (const QString& k : keys) {
            QVERIFY2(!exists(k), qPrintable(k));
            QVERIFY2(exists(m_root + QStringLiteral("dir new/") + k.mid(folder.size())), qPrintable(k));
        }
    }

    // Copy a folder, copy it again (everything exists: skipped), move the
    // copy; into QBUCKET_IT_SECOND_BUCKET too when set (another qbucket-it-*
    // bucket of the account; what lands there is removed again).
    void copyAndMoveAFolder()
    {
        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));
        const QString src = m_root + QStringLiteral("copy src/");
        const QStringList rel = {QString(), QStringLiteral("a.txt"), QStringLiteral("sub/b+c ä.txt")};
        for (const QString& r : rel)
            QVERIFY(put(src + r, r.isEmpty() ? QByteArray() : r.toUtf8()));
        const QList<CopyJob::Item> items{{src, true}};
        Job* job = run(new CopyJob(m_account, m_bucket, items, m_root, m_bucket, m_root + QStringLiteral("copy dst/"),
                                   CopyJob::Mode::Copy, m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, rel.size());
        const QString copied = m_root + QStringLiteral("copy dst/copy src/");
        for (const QString& r : rel)
            QVERIFY2(exists(copied + r) && exists(src + r), qPrintable(r));

        // Again: the two files exist (skipped), the marker is copied over itself.
        job = run(new CopyJob(m_account, m_bucket, items, m_root, m_bucket, m_root + QStringLiteral("copy dst/"),
                              CopyJob::Mode::Copy, m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().skipped, 2);

        // Move the copy: copied again, checked, the originals deleted.
        const QList<CopyJob::Item> moveItems{{copied, true}};
        job = run(new CopyJob(m_account, m_bucket, moveItems, m_root + QStringLiteral("copy dst/"), m_bucket,
                              m_root + QStringLiteral("moved/"), CopyJob::Mode::Move, m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        for (const QString& r : rel) {
            QVERIFY2(!exists(copied + r), qPrintable(r));
            QVERIFY2(exists(m_root + QStringLiteral("moved/copy src/") + r), qPrintable(r));
        }

        const QString second = qEnvironmentVariable("QBUCKET_IT_SECOND_BUCKET");
        if (second.isEmpty())
            return;
        static const QRegularExpression safeName(QStringLiteral("^qbucket-it-[a-z0-9-]+$"));
        QVERIFY2(safeName.match(second).hasMatch() && second != m_bucket, "QBUCKET_IT_SECOND_BUCKET must be another qbucket-it-* bucket");
        job = run(new CopyJob(m_account, m_bucket, items, m_root, second, m_root, CopyJob::Mode::Move, m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        for (const QString& r : rel)
            QVERIFY2(!exists(src + r), qPrintable(r));
        // Remove what landed in the second bucket (only this run's keys).
        for (const QString& r : rel) {
            const QString k = m_root + QStringLiteral("copy src/") + r;
            QVERIFY(k.startsWith(m_root));
            std::optional<bool> gone;
            QObject ctx;
            session()->deleteObject(second, k, QString(), &ctx, [&](const s3::Result<s3::DeleteObjectResult>& d) { gone = bool(d); });
            QVERIFY(QTest::qWaitFor([&] { return gone.has_value(); }, 30000) && *gone);
        }
    }

    // Headers and metadata in place: one object, a folder; an object with
    // tags and a public ACL keeps both where the provider confirms tag and
    // ACL writes (Hetzner drops the tags on such a copy: they are written
    // back), and is left alone where it doesn't (a generic server).
    void editMetadataInPlace()
    {
        m_ops->setPlanDir(m_dir.filePath(QStringLiteral("plans")));
        const QString one = m_root + QStringLiteral("edit/one.txt");
        QVERIFY(put(one, "edit me"));
        EditObjectsJob::ChangeSpec change;
        change.cacheControl = QStringLiteral("max-age=60");
        change.metadata.insert(QStringLiteral("stage"), QStringLiteral("it"));
        Job* job = run(new EditObjectsJob(m_account, m_bucket, {{one, false}}, change, m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        const std::optional<s3::ObjectMeta> head = meta(one);
        QVERIFY(head);
        QCOMPARE(head->cacheControl, QStringLiteral("max-age=60"));
        QCOMPARE(head->userMetadata.value(QStringLiteral("stage")), QStringLiteral("it"));
        QCOMPARE(head->size, qint64(7));

        QVERIFY(put(m_root + QStringLiteral("edit/two.txt"), "second"));
        EditObjectsJob::ChangeSpec lang;
        lang.contentLanguage = QStringLiteral("de");
        job = run(new EditObjectsJob(m_account, m_bucket, {{m_root + QStringLiteral("edit/"), true}}, lang, m_ops->planDir()));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, 2);
        QCOMPARE(meta(one)->cacheControl, QStringLiteral("max-age=60"));   // untouched by the second edit
        QCOMPARE(meta(one)->contentLanguage, QStringLiteral("de"));

        // Tags and a public ACL — only where the server has them.
        const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(m_account.providerId);
        const QString tagged = m_root + QStringLiteral("edit-tagged/obj.txt");
        QVERIFY(put(tagged, "tagged"));
        s3::PutObjectTaggingParams tp;
        tp.bucket = m_bucket;
        tp.key = tagged;
        tp.tags = {{QStringLiteral("keep"), QStringLiteral("me")}};
        std::optional<bool> tagOk;
        QObject ctx;
        session()->putObjectTagging(tp, &ctx, [&](const s3::Result<s3::SubresourceWriteResult>& r) { tagOk = bool(r); });
        QVERIFY(QTest::qWaitFor([&] { return tagOk.has_value(); }, 30000));
        if (!*tagOk)
            QSKIP("no object tags on this backend");
        const bool confirmed = profile->capabilities().state(s3::Feature::ObjectTaggingWrite) == s3::CapabilityState::Supported
            && profile->capabilities().state(s3::Feature::ObjectAclWrite) == s3::CapabilityState::Supported;
        if (confirmed) {
            s3::PutObjectAclParams ap;
            ap.bucket = m_bucket;
            ap.key = tagged;
            ap.cannedAcl = QStringLiteral("public-read");
            std::optional<bool> aclOk;
            session()->putObjectAcl(ap, &ctx, [&](const s3::Result<s3::SubresourceWriteResult>& r) { aclOk = bool(r); });
            QVERIFY(QTest::qWaitFor([&] { return aclOk.has_value(); }, 30000) && *aclOk);
        }
        job = run(new EditObjectsJob(m_account, m_bucket, {{tagged, false}}, change, m_ops->planDir()));
        if (!confirmed) {
            QCOMPARE(job->report().skipped, 1);
            QVERIFY(job->report().notes.join(u' ').contains(QLatin1String("tags")));
            return;
        }
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().failures.join(u'|')));
        QCOMPARE(meta(tagged)->cacheControl, QStringLiteral("max-age=60"));
        std::optional<s3::Result<s3::ObjectTagging>> tags;
        session()->getObjectTagging(m_bucket, tagged, QString(), &ctx, [&](s3::Result<s3::ObjectTagging> r) { tags.emplace(std::move(r)); });
        QVERIFY(QTest::qWaitFor([&] { return tags.has_value(); }, 30000) && *tags);
        QCOMPARE((*tags)->tags.size(), 1);
        std::optional<s3::Result<s3::AccessControlPolicy>> acl;
        session()->getObjectAcl(m_bucket, tagged, QString(), &ctx, [&](s3::Result<s3::AccessControlPolicy> r) { acl.emplace(std::move(r)); });
        QVERIFY(QTest::qWaitFor([&] { return acl.has_value(); }, 30000) && *acl);
        QCOMPARE(s3::cannedAclOf(**acl), std::optional<QString>(QStringLiteral("public-read")));
        // Not public any longer than needed.
        s3::PutObjectAclParams back;
        back.bucket = m_bucket;
        back.key = tagged;
        back.cannedAcl = QStringLiteral("private");
        std::optional<bool> privateOk;
        session()->putObjectAcl(back, &ctx, [&](const s3::Result<s3::SubresourceWriteResult>& r) { privateOk = bool(r); });
        QVERIFY(QTest::qWaitFor([&] { return privateOk.has_value(); }, 30000) && *privateOk);
    }

    // Tags and a canned ACL through their jobs — where the provider confirms
    // such writes; refused elsewhere (a generic server).
    void tagAndAclAnObject()
    {
        const QString k = m_root + QStringLiteral("props/obj.txt");
        QVERIFY(put(k, "props"));
        const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(m_account.providerId);
        const bool confirmed = profile->capabilities().state(s3::Feature::ObjectTaggingWrite) == s3::CapabilityState::Supported;
        Job* job = run(ObjectPropertyJob::tags(m_account, m_bucket, k, {{QStringLiteral("it"), QStringLiteral("yes")}}));
        if (!confirmed) {
            QCOMPARE(job->outcome(), Job::Outcome::Refused);
            return;
        }
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY2(job->report().verified, qPrintable(job->report().verification));
        job = run(ObjectPropertyJob::acl(m_account, m_bucket, k, QStringLiteral("public-read")));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QVERIFY(job->report().verified);
        job = run(ObjectPropertyJob::acl(m_account, m_bucket, k, QStringLiteral("private")));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        job = run(ObjectPropertyJob::tags(m_account, m_bucket, k, {}));
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(meta(k)->size, qint64(5));   // the data never changed
    }

    // A folder tree with an empty folder and a large file (multipart;
    // QBUCKET_IT_UPLOAD_MIB, default 70), then the same again: everything
    // exists, nothing is overwritten unasked.
    void uploadAFolder()
    {
        const QString local = m_dir.filePath(QStringLiteral("upload/tree ä"));
        QDir().mkpath(local + QStringLiteral("/sub/deeper"));
        QDir().mkpath(local + QStringLiteral("/empty"));
        QStringList small;
        for (const QString& rel : {QStringLiteral("a.txt"), QStringLiteral("sub/b c.txt"), QStringLiteral("sub/deeper/ü+%.bin")}) {
            QFile f(local + u'/' + rel);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(rel.toUtf8().repeated(100));
            small.append(rel);
        }
        bool ok = false;
        const qint64 mib = qEnvironmentVariableIntValue("QBUCKET_IT_UPLOAD_MIB", &ok);
        const qint64 bigSize = (ok && mib > 0 ? mib : 70) * 1024 * 1024 + 17;
        QByteArray chunk(qsizetype(1024) * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < chunk.size(); ++i)
            chunk[i] = char((i * 31 + 7) & 0xFF);
        {
            QFile f(local + QStringLiteral("/big.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            for (qint64 written = 0; written < bigSize;) {
                const qint64 n = std::min<qint64>(chunk.size(), bigSize - written);
                QCOMPARE(f.write(chunk.constData(), n), n);
                written += n;
            }
        }
        const QString target = m_root + QStringLiteral("uploaded/");
        auto* job = new UploadJob(m_account, m_bucket, target, {local}, m_ops->planDir(), {});
        run(job);
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(job->report().succeeded, small.size() + 2);   // + big.bin + the empty folder
        qInfo().noquote() << job->report().verification;
        const QString base = target + QStringLiteral("tree ä/");
        for (const QString& rel : small)
            QVERIFY2(exists(base + rel), qPrintable(rel));
        QVERIFY(exists(base + QStringLiteral("empty/")));
        std::optional<s3::Result<s3::ObjectMeta>> head;
        QObject ctx;
        session()->headObject(m_bucket, base + QStringLiteral("big.bin"), QString(), &ctx,
                              [&](s3::Result<s3::ObjectMeta> r) { head.emplace(std::move(r)); });
        QTRY_VERIFY_WITH_TIMEOUT(head.has_value(), 30000);
        QVERIFY(*head);
        QCOMPARE((*head)->size, bigSize);
        QVERIFY((*head)->etag.contains(u'-'));   // multipart

        auto* again = new UploadJob(m_account, m_bucket, target, {local}, m_ops->planDir(), {});
        run(again);
        QCOMPARE(again->outcome(), Job::Outcome::Succeeded);
        QCOMPARE(again->report().succeeded, 0);
        QCOMPARE(again->preview().conflicts, small.size() + 1);
    }

    // The tree of uploadAFolder back to disk, byte for byte.
    void downloadTheUploadBack()
    {
        const QString source = m_dir.filePath(QStringLiteral("upload/tree ä"));
        if (!QFile::exists(source + QStringLiteral("/big.bin")))
            QSKIP("needs uploadAFolder");
        const QString target = m_dir.filePath(QStringLiteral("download"));
        auto* job = new DownloadJob(m_account, m_bucket, {{m_root + QStringLiteral("uploaded/tree ä/"), true, {}}}, target,
                                    m_ops->planDir(), {});
        run(job);
        QCOMPARE(job->outcome(), Job::Outcome::Succeeded);
        qInfo().noquote() << job->report().verification;
        QCOMPARE(job->sizeVerified(), 0);   // MD5 or MD5-of-MD5s for every file
        for (const QString& rel : {QStringLiteral("a.txt"), QStringLiteral("sub/b c.txt"),
                                   QStringLiteral("sub/deeper/ü+%.bin"), QStringLiteral("big.bin")}) {
            QFile a(source + u'/' + rel);
            QFile b(target + QStringLiteral("/tree ä/") + rel);
            QVERIFY2(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly), qPrintable(rel));
            QCryptographicHash ha(QCryptographicHash::Md5);
            QCryptographicHash hb(QCryptographicHash::Md5);
            QVERIFY(ha.addData(&a));
            QVERIFY(hb.addData(&b));
            QCOMPARE(hb.result(), ha.result());
        }
        QVERIFY(QFileInfo(target + QStringLiteral("/tree ä/empty")).isDir());
    }

private:
    bool put(const QString& key, const QByteArray& data)
    {
        s3::PutObjectParams p;
        p.bucket = m_bucket;
        p.key = key;
        p.data = data;
        std::optional<bool> ok;
        QObject ctx;
        session()->putObject(p, &ctx, [&](const s3::Result<s3::PutObjectResult>& r) { ok = bool(r); });
        return QTest::qWaitFor([&] { return ok.has_value(); }, 30000) && *ok;
    }

    std::optional<s3::ObjectMeta> meta(const QString& key)
    {
        std::optional<s3::Result<s3::ObjectMeta>> r;
        QObject ctx;
        session()->headObject(m_bucket, key, QString(), &ctx, [&](s3::Result<s3::ObjectMeta> m) { r.emplace(std::move(m)); });
        if (!QTest::qWaitFor([&] { return r.has_value(); }, 30000) || !*r)
            return std::nullopt;
        return **r;
    }

    bool exists(const QString& key)
    {
        std::optional<bool> found;
        QObject ctx;
        session()->headObject(m_bucket, key, QString(), &ctx, [&](const s3::Result<s3::ObjectMeta>& r) { found = bool(r); });
        return QTest::qWaitFor([&] { return found.has_value(); }, 30000) && *found;
    }

    s3::Session* session()
    {
        const s3::Result<s3::Session*> s = m_sessions->session(m_account.id);
        return s ? *s : nullptr;
    }

    // Starts a job, confirms its preview, waits for the end.
    Job* run(Job* job)
    {
        QObject::connect(m_ops.get(), &OperationController::previewNeeded, job, [job](Job* j) {
            if (j == job)
                j->confirm();
        });
        QSignalSpy done(job, &Job::finished);
        m_ops->start(job);
        if (!done.wait(120000))
            qWarning("job did not finish");
        if (job->outcome() != Job::Outcome::Succeeded)
            qWarning().noquote() << OperationController::summary(*job) << job->report().failures;
        return job;
    }

    QTemporaryDir m_dir;
    QString m_bucket;
    QString m_root;
    Account m_account;
    std::unique_ptr<AccountStore> m_accounts;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<AccountSessions> m_sessions;
    std::unique_ptr<CapabilityStore> m_store;
    std::unique_ptr<ActivityLog> m_log;
    std::unique_ptr<CapabilityController> m_caps;
    std::unique_ptr<Journal> m_journal;
    std::unique_ptr<TransferService> m_transfers;
    std::unique_ptr<OperationController> m_ops;
};

QTEST_GUILESS_MAIN(ItOperations)
#include "it_operations.moc"
