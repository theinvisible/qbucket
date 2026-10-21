// Versions through the Session against the versioned FakeObjectStore:
// ListObjectVersions (pages, markers, delimiter) with ListObjectVersionsPager,
// delete markers on HEAD / GET, reading, copying and deleting a version,
// batch deletes with version ids, locked versions, the "null" version.

#include <qbucket/s3/ListObjectVersionsPager.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>

#include <QSignalSpy>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;
using namespace qbucket::s3::testing;

namespace {

const QString kBucket = QStringLiteral("bkt");

Credentials testCredentials()
{
    Credentials c;
    c.accessKeyId = QStringLiteral("AKIDTEST");
    c.secretAccessKey = QStringLiteral("test/secret+key");
    return c;
}

} // namespace

class TestVersioning : public QObject {
    Q_OBJECT

private:
    FakeS3Server* m_server = nullptr;
    std::unique_ptr<FakeObjectStore> m_store;
    std::unique_ptr<Session> m_session;
    QObject m_context;

    // Runs one Session call and waits for its answer.
    template <typename T, typename Call>
    Result<T> call(Call&& start)
    {
        std::optional<Result<T>> out;
        start([&out](Result<T> r) { out.emplace(std::move(r)); });
        if (!QTest::qWaitFor([&] { return out.has_value(); }, 10000))
            return Error::make(ErrorCategory::Internal, QStringLiteral("no answer"));
        return std::move(*out);
    }

    Result<ObjectMeta> head(const QString& key, const QString& versionId = {})
    {
        return call<ObjectMeta>([&](auto cb) { m_session->headObject(kBucket, key, versionId, &m_context, cb); });
    }

    Result<DeleteObjectResult> remove(const QString& key, const QString& versionId = {})
    {
        return call<DeleteObjectResult>(
            [&](auto cb) { m_session->deleteObject(kBucket, key, versionId, &m_context, cb); });
    }

    // Every entry of a listing, walked with the pager.
    QList<VersionEntry> walk(ListObjectVersionsParams p, QStringList* prefixes = nullptr, int* pages = nullptr)
    {
        p.bucket = kBucket;
        ListObjectVersionsPager pager(*m_session, p);
        QList<VersionEntry> out;
        while (pager.hasMore()) {
            bool got = false;
            pager.fetchNext(&m_context, [&](Result<ListObjectVersionsPage> r) {
                if (r) {
                    out += r->entries;
                    if (prefixes)
                        *prefixes += r->commonPrefixes;
                } else {
                    qWarning("%s", qPrintable(r.error().toString()));
                }
                got = true;
            });
            if (!QTest::qWaitFor([&] { return got; }, 10000))
                break;
        }
        if (pages)
            *pages = pager.pagesFetched();
        return out;
    }

private Q_SLOTS:
    void init()
    {
        m_server = new FakeS3Server(this);
        QVERIFY(m_server->listen());
        m_server->setVerifySignatures(testCredentials(), "test-1");
        m_store = std::make_unique<FakeObjectStore>(m_server);
        m_store->createBucket(kBucket);
        SessionOptions o;
        o.endpoint.url = m_server->endpointUrl();
        o.endpoint.region = QStringLiteral("test-1");
        o.profile = std::make_shared<GenericS3Profile>();
        o.credentials = std::make_shared<StaticCredentialsProvider>(testCredentials());
        o.retryBaseDelayMs = 5;
        m_session = std::make_unique<Session>(o);
    }

    void cleanup()
    {
        m_session.reset();
        m_store.reset();
        delete m_server;
        m_server = nullptr;
    }

    void unversionedBucketHasOneNullVersion()
    {
        m_store->putObject(kBucket, QStringLiteral("a"), "one");
        m_store->putObject(kBucket, QStringLiteral("a"), "two");
        const QList<VersionEntry> all = walk({});
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].versionId, QStringLiteral("null"));
        QVERIFY(all[0].isLatest);
        const Result<ObjectMeta> meta = head(QStringLiteral("a"));
        QVERIFY(meta);
        QVERIFY(meta->versionId.isEmpty());   // no x-amz-version-id without versioning
        const Result<DeleteObjectResult> del = remove(QStringLiteral("a"));
        QVERIFY(del);
        QVERIFY(!del->deleteMarker);
        QVERIFY(m_store->versionedKeys(kBucket).isEmpty());
    }

    void writesAddVersionsAndDeletesAddMarkers()
    {
        m_store->setVersioning(kBucket, QStringLiteral("Enabled"));
        const QString v1 = m_store->putObject(kBucket, QStringLiteral("doc"), "first");
        const QString v2 = m_store->putObject(kBucket, QStringLiteral("doc"), "second!");
        QVERIFY(v1 != v2 && v1 != QLatin1String("null"));

        Result<ObjectMeta> meta = head(QStringLiteral("doc"));
        QVERIFY(meta);
        QCOMPARE(meta->versionId, v2);
        QCOMPARE(meta->size, 7);

        // The older version is still there, by its id.
        const Result<ObjectData> old = call<ObjectData>(
            [&](auto cb) { m_session->getObject(kBucket, QStringLiteral("doc"), v1, &m_context, cb); });
        QVERIFY(old);
        QCOMPARE(old->data, QByteArray("first"));
        QCOMPARE(old->meta.versionId, v1);

        // A delete hides the key behind a marker.
        const Result<DeleteObjectResult> del = remove(QStringLiteral("doc"));
        QVERIFY(del);
        QVERIFY(del->deleteMarker);
        const QString marker = del->versionId;
        QVERIFY(!marker.isEmpty());
        meta = head(QStringLiteral("doc"));
        QCOMPARE(meta.error().category, ErrorCategory::NotFound);
        QVERIFY(meta.error().deleteMarker);
        QCOMPARE(meta.error().versionId, marker);
        meta = head(QStringLiteral("doc"), marker);   // the marker version itself: 405
        QCOMPARE(meta.error().category, ErrorCategory::NotFound);
        QVERIFY(meta.error().deleteMarker);
        QVERIFY(m_store->keys(kBucket).isEmpty());

        // Not in ListObjects, but in ListObjectVersions, newest first.
        const Result<ListPage> plain = call<ListPage>([&](auto cb) {
            ListObjectsParams p;
            p.bucket = kBucket;
            m_session->listObjects(p, &m_context, cb);
        });
        QVERIFY(plain);
        QVERIFY(plain->objects.isEmpty());
        const QList<VersionEntry> all = walk({});
        QCOMPARE(all.size(), 3);
        QVERIFY(all[0].isDeleteMarker && all[0].isLatest);
        QCOMPARE(all[0].versionId, marker);
        QCOMPARE(all[1].versionId, v2);
        QVERIFY(!all[1].isLatest);
        QCOMPARE(all[2].versionId, v1);
        QCOMPARE(all[2].size, 5);

        // Removing the marker brings the object back (undelete).
        const Result<DeleteObjectResult> undelete = remove(QStringLiteral("doc"), marker);
        QVERIFY(undelete);
        QVERIFY(undelete->deleteMarker);
        QCOMPARE(undelete->versionId, marker);
        meta = head(QStringLiteral("doc"));
        QVERIFY(meta);
        QCOMPARE(meta->versionId, v2);
    }

    void copyAVersionOverTheCurrentOne()
    {
        m_store->setVersioning(kBucket, QStringLiteral("Enabled"));
        const QString v1 = m_store->putObject(kBucket, QStringLiteral("doc"), "first", QStringLiteral("text/plain"));
        m_store->putObject(kBucket, QStringLiteral("doc"), "second");
        CopyObjectParams p;
        p.sourceBucket = kBucket;
        p.sourceKey = QStringLiteral("doc");
        p.sourceVersionId = v1;
        p.bucket = kBucket;
        p.key = QStringLiteral("doc");
        const Result<CopyObjectResult> r =
            call<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_context, cb); });
        QVERIFY2(r, qPrintable(r ? QString() : r.error().toString()));
        QCOMPARE(r->sourceVersionId, v1);
        QVERIFY(!r->versionId.isEmpty() && r->versionId != v1);
        QCOMPARE(m_store->object(kBucket, QStringLiteral("doc"))->data, QByteArray("first"));
        QCOMPARE(m_store->versions(kBucket, QStringLiteral("doc")).size(), 3);   // the restore is a new version

        // A delete marker can't be a copy source.
        const QString marker = m_store->deleteObject(kBucket, QStringLiteral("doc"));
        p.sourceVersionId = marker;
        const Result<CopyObjectResult> fromMarker =
            call<CopyObjectResult>([&](auto cb) { m_session->copyObject(p, &m_context, cb); });
        QVERIFY(!fromMarker);
        QCOMPARE(fromMarker.error().category, ErrorCategory::InvalidRequest);
    }

    void deleteVersionsOneByOneAndInBatches()
    {
        m_store->setVersioning(kBucket, QStringLiteral("Enabled"));
        const QString a1 = m_store->putObject(kBucket, QStringLiteral("a"), "1");
        const QString a2 = m_store->putObject(kBucket, QStringLiteral("a"), "22");
        const QString b1 = m_store->putObject(kBucket, QStringLiteral("b"), "3");
        const QString bMarker = m_store->deleteObject(kBucket, QStringLiteral("b"));
        m_store->lockVersion(kBucket, QStringLiteral("a"), a1);

        // Deleting the current version makes the previous one current.
        Result<DeleteObjectResult> del = remove(QStringLiteral("a"), a2);
        QVERIFY(del);
        QVERIFY(!del->deleteMarker);
        QCOMPARE(head(QStringLiteral("a"))->versionId, a1);
        QCOMPARE(head(QStringLiteral("a"), a2).error().category, ErrorCategory::NotFound);

        // A locked version can't be deleted.
        del = remove(QStringLiteral("a"), a1);
        QVERIFY(!del);
        QCOMPARE(del.error().category, ErrorCategory::NotPermitted);

        const Result<DeleteObjectsResult> batch = call<DeleteObjectsResult>([&](auto cb) {
            m_session->deleteObjects(kBucket,
                                     {{QStringLiteral("b"), bMarker}, {QStringLiteral("b"), b1}, {QStringLiteral("a"), a1}},
                                     &m_context, cb);
        });
        QVERIFY2(batch, qPrintable(batch ? QString() : batch.error().toString()));
        QCOMPARE(batch->deleted.size(), 2);
        QVERIFY(batch->deleted[0].deleteMarker);
        QCOMPARE(batch->deleted[0].versionId, bMarker);
        QCOMPARE(batch->deleted[1].versionId, b1);
        QCOMPARE(batch->failed.size(), 1);
        QCOMPARE(batch->failed[0].versionId, a1);
        QCOMPARE(batch->failed[0].code, QStringLiteral("AccessDenied"));
        QCOMPARE(m_store->versionedKeys(kBucket), QStringList{QStringLiteral("a")});
    }

    void suspendedVersioningReplacesTheNullVersion()
    {
        m_store->setVersioning(kBucket, QStringLiteral("Enabled"));
        const QString v1 = m_store->putObject(kBucket, QStringLiteral("k"), "kept");
        m_store->setVersioning(kBucket, QStringLiteral("Suspended"));
        QCOMPARE(m_store->putObject(kBucket, QStringLiteral("k"), "null 1"), QStringLiteral("null"));
        m_store->putObject(kBucket, QStringLiteral("k"), "null 2");   // overwrites the null version
        QList<FakeObjectStore::Version> h = m_store->versions(kBucket, QStringLiteral("k"));
        QCOMPARE(h.size(), 2);
        QCOMPARE(h[0].object.data, QByteArray("null 2"));
        QCOMPARE(h[1].versionId, v1);
        // A delete while suspended: a "null" delete marker replaces the null version.
        const Result<DeleteObjectResult> del = remove(QStringLiteral("k"));
        QVERIFY(del && del->deleteMarker);
        QCOMPARE(del->versionId, QStringLiteral("null"));
        h = m_store->versions(kBucket, QStringLiteral("k"));
        QCOMPARE(h.size(), 2);
        QVERIFY(h[0].deleteMarker);
    }

    void listingPagesSplitAKeysVersions()
    {
        m_store->setVersioning(kBucket, QStringLiteral("Enabled"));
        QStringList ids;
        for (int i = 0; i < 5; ++i)
            ids.prepend(m_store->putObject(kBucket, QStringLiteral("f/x"), QByteArray::number(i)));
        m_store->putObject(kBucket, QStringLiteral("f/y"), "y");
        m_store->deleteObject(kBucket, QStringLiteral("f/y"));
        m_store->putObject(kBucket, QStringLiteral("f/sub/z"), "z");
        m_store->putObject(kBucket, QStringLiteral("g"), "outside");

        ListObjectVersionsParams p;
        p.prefix = QStringLiteral("f/");
        p.maxKeys = 2;
        int pages = 0;
        QList<VersionEntry> all = walk(p, nullptr, &pages);
        QCOMPARE(all.size(), 8);   // 5 + 2 (y and its marker) + 1
        QCOMPARE(pages, 4);
        QCOMPARE(all[0].key, QStringLiteral("f/sub/z"));
        QStringList xIds;
        for (const VersionEntry& e : std::as_const(all)) {
            if (e.key == QLatin1String("f/x"))
                xIds.append(e.versionId);
        }
        QCOMPARE(xIds, ids);   // newest first, across page boundaries
        QVERIFY(all.last().key == QLatin1String("f/y") && !all.last().isDeleteMarker);

        // With a delimiter: sub/ is one common prefix.
        p.delimiter = QStringLiteral("/");
        QStringList prefixes;
        all = walk(p, &prefixes, &pages);
        QCOMPARE(prefixes, QStringList{QStringLiteral("f/sub/")});
        QCOMPARE(all.size(), 7);
        QVERIFY(std::none_of(all.cbegin(), all.cend(), [](const VersionEntry& e) { return e.key.contains(u"sub"); }));
    }

    void bucketVersioningRoundTrip()
    {
        const auto get = [&] {
            return call<BucketVersioning>([&](auto cb) { m_session->getBucketVersioning(kBucket, &m_context, cb); });
        };
        Result<BucketVersioning> v = get();
        QVERIFY(v);
        QCOMPARE(v->status, VersioningStatus::Never);
        QVERIFY(!v->mfaDelete);

        PutBucketVersioningParams p;
        p.bucket = kBucket;
        p.status = VersioningStatus::Enabled;
        QVERIFY(call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); }));
        const RecordedRequest put = m_server->requests().last();
        QVERIFY(put.hasQueryItem("versioning"));
        QVERIFY(!put.header("content-md5").isEmpty());
        QVERIFY(put.body.contains("<Status>Enabled</Status>"));
        QVERIFY(!put.body.contains("MfaDelete"));
        v = get();
        QCOMPARE(v->status, VersioningStatus::Enabled);
        QCOMPARE(v->mfaDelete, std::optional(false));

        p.status = VersioningStatus::Suspended;
        QVERIFY(call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); }));
        QCOMPARE(get()->status, VersioningStatus::Suspended);
        QCOMPARE(m_store->versioning(kBucket), QStringLiteral("Suspended"));

        // "Never" can't be sent; refused before anything leaves.
        p.status = VersioningStatus::Never;
        const int sent = m_server->requestCount();
        const auto never = call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); });
        QCOMPARE(never.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(m_server->requestCount(), sent);
    }

    // A proxy that drops ?versioning turns the PUT into CreateBucket.
    void versioningWriteAnsweredLikeCreateBucketIsAnError()
    {
        PutBucketVersioningParams p;
        p.bucket = kBucket;
        m_server->addRoute({"PUT", {}, {"versioning"}, {}},
                           {FakeResponse::s3Error(409, "BucketAlreadyOwnedByYou", "Your previous request to create "
                                                                                  "the named bucket succeeded"),
                            FakeResponse::ok().withHeader("Location", "/bkt")},
                           2);
        auto r = call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
        QVERIFY(r.error().message.contains(QLatin1String("CreateBucket")));
        r = call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::Protocol);
        QCOMPARE(m_store->versioning(kBucket), QString());   // nothing reached the store
    }

    void versioningWriteFeatureIsCheckedAndLearnedSeparately()
    {
        CapabilitySet set;
        set.scope = CapabilityScope::Server;
        set.set(Feature::BucketVersioningWrite,
                {CapabilityState::Unsupported, CapabilitySource::Profile, {}, 0, {}, false, 0});
        m_session->setCapabilities(set);
        PutBucketVersioningParams p;
        p.bucket = kBucket;
        const int sent = m_server->requestCount();
        auto r = call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QCOMPARE(m_server->requestCount(), sent);
        QVERIFY(call<BucketVersioning>([&](auto cb) { m_session->getBucketVersioning(kBucket, &m_context, cb); }));

        // A 501 on the write teaches the write feature only.
        m_session->setCapabilities(CapabilitySet());
        m_server->addRoute({"PUT", {}, {"versioning"}, {}}, {FakeResponse::s3Error(501, "NotImplemented")}, 1);
        QSignalSpy learned(m_session.get(), &Session::capabilityLearned);
        r = call<SubresourceWriteResult>([&](auto cb) { m_session->putBucketVersioning(p, &m_context, cb); });
        QCOMPARE(r.error().category, ErrorCategory::Unsupported);
        QTRY_COMPARE(learned.size(), 1);
        QCOMPARE(learned.first().at(1).toString(), QStringLiteral("bucket.versioning-write"));
    }

    void pagerStopsOnALoop()
    {
        // Two pages that hand back each other's markers.
        const QByteArray page1 = "<ListVersionsResult><IsTruncated>true</IsTruncated>"
                                 "<Version><Key>a</Key><VersionId>1</VersionId></Version>"
                                 "<NextKeyMarker>a</NextKeyMarker><NextVersionIdMarker>1</NextVersionIdMarker>"
                                 "</ListVersionsResult>";
        const QByteArray page2 = "<ListVersionsResult><IsTruncated>true</IsTruncated>"
                                 "<Version><Key>b</Key><VersionId>2</VersionId></Version>"
                                 "<NextKeyMarker>b</NextKeyMarker><NextVersionIdMarker>2</NextVersionIdMarker>"
                                 "</ListVersionsResult>";
        Matcher list;
        list.method = "GET";
        m_server->addRoute(list, {FakeResponse::ok(page1), FakeResponse::ok(page2), FakeResponse::ok(page1)});
        ListObjectVersionsParams p;
        p.bucket = kBucket;
        ListObjectVersionsPager pager(*m_session, p);
        std::optional<Error> error;
        int pages = 0;
        while (pager.hasMore() && pages < 10) {
            bool got = false;
            QVERIFY(pager.fetchNext(&m_context, [&](const Result<ListObjectVersionsPage>& r) {
                if (!r)
                    error = r.error();
                got = true;
            }) != RequestId::Invalid);
            QCOMPARE(pager.fetchNext(&m_context, [](const Result<ListObjectVersionsPage>&) {}), RequestId::Invalid);
            QTRY_VERIFY(got);
            ++pages;
        }
        QVERIFY(error.has_value());
        QCOMPARE(error->category, ErrorCategory::Protocol);
        QCOMPARE(pages, 3);
        QCOMPARE(m_server->requests()[1].queryItem("key-marker"), QByteArray("a"));
        QCOMPARE(m_server->requests()[1].queryItem("version-id-marker"), QByteArray("1"));
    }
};

QTEST_GUILESS_MAIN(TestVersioning)
#include "test_versioning.moc"
