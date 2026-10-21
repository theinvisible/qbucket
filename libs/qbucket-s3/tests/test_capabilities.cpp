#include <qbucket/s3/Capabilities.h>

#include <QtTest>

using namespace qbucket::s3;

namespace {

CapabilityEntry probed(CapabilityState s, int status = 200, int unknownAttempts = 0,
                       const QDateTime& at = QDateTime::currentDateTimeUtc())
{
    return {s, CapabilitySource::Probed, at, status, {}, false, unknownAttempts};
}

CapabilityEntry profileSays(CapabilityState s)
{
    return {s, CapabilitySource::Profile, {}, 0, {}, false, 0};
}

} // namespace

class TestCapabilities : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void idsAreStableAndUnique()
    {
        QSet<QString> ids;
        for (const Feature f : allFeatures()) {
            const QString id = featureId(f);
            QVERIFY(!ids.contains(id));
            ids.insert(id);
            QCOMPARE(featureFromId(id), std::optional(f));
        }
        QCOMPARE(ids.size(), int(Feature::LegalHoldWrite) + 1);
        // Persisted ids must never change.
        QCOMPARE(featureId(Feature::BucketVersioning), QStringLiteral("bucket.versioning"));
        QCOMPARE(featureId(Feature::CrossBucketCopy), QStringLiteral("object.cross-bucket-copy"));
        QVERIFY(!featureFromId(QStringLiteral("bucket.teleport")));
        QCOMPARE(featuresFor(Operation::DeleteObjects), QList<Feature>{Feature::BatchDelete});
        QCOMPARE(featureId(Feature::MultipartUpload), QStringLiteral("object.multipart-upload"));
        QCOMPARE(featuresFor(Operation::UploadPart), QList<Feature>{Feature::MultipartUpload});
        QCOMPARE(featuresFor(Operation::ListMultipartUploads), QList<Feature>{Feature::MultipartListing});
        // Writes need their own write feature first (what a 501 teaches),
        // then the read feature.
        QCOMPARE(featureId(Feature::ObjectTaggingWrite), QStringLiteral("object.tagging-write"));
        QCOMPARE(featureId(Feature::ObjectAclWrite), QStringLiteral("object.acl-write"));
        QCOMPARE(featuresFor(Operation::GetObjectTagging), QList<Feature>{Feature::ObjectTagging});
        QCOMPARE(featuresFor(Operation::PutObjectTagging),
                 (QList<Feature>{Feature::ObjectTaggingWrite, Feature::ObjectTagging}));
        QCOMPARE(featuresFor(Operation::PutObjectAcl), (QList<Feature>{Feature::ObjectAclWrite, Feature::ObjectAcl}));
        QCOMPARE(featureId(Feature::BucketVersioningWrite), QStringLiteral("bucket.versioning-write"));
        QCOMPARE(featuresFor(Operation::PutBucketVersioning),
                 (QList<Feature>{Feature::BucketVersioningWrite, Feature::BucketVersioning}));
        QCOMPARE(featuresFor(Operation::GetBucketVersioning), QList<Feature>{Feature::BucketVersioning});
        QCOMPARE(featuresFor(Operation::ListObjectVersions), QList<Feature>{Feature::ObjectVersionsListing});
        QVERIFY(isMutating(Operation::PutObjectTagging));
        QVERIFY(!isMutating(Operation::GetObjectAcl));
    }

    void probeWinsOverProfile()
    {
        CapabilitySet probe;
        CapabilitySet profile;
        profile.set(Feature::Website, profileSays(CapabilityState::Unsupported));
        probe.set(Feature::Website, probed(CapabilityState::Supported));
        QCOMPARE(capabilities::effective(Feature::Website, probe, profile).state, CapabilityState::Supported);

        // An Unknown probe falls back to the profile.
        probe.set(Feature::Website, probed(CapabilityState::Unknown, 503, 1));
        const CapabilityEntry e = capabilities::effective(Feature::Website, probe, profile);
        QCOMPARE(e.state, CapabilityState::Unsupported);
        QCOMPARE(e.source, CapabilitySource::Profile);
    }

    void unknownNeverUnlocksDestructiveOperations()
    {
        const CapabilitySet none;
        QCOMPARE(capabilities::decide(Feature::Lifecycle, none, none, false), Decision::AllowedUnverified);
        QCOMPARE(capabilities::decide(Feature::Lifecycle, none, none, true), Decision::Blocked);

        CapabilitySet probe;
        probe.set(Feature::Lifecycle, probed(CapabilityState::Supported));
        QCOMPARE(capabilities::decide(Feature::Lifecycle, probe, none, true), Decision::Allowed);
        probe.set(Feature::Lifecycle, probed(CapabilityState::NotPermitted, 403));
        QCOMPARE(capabilities::decide(Feature::Lifecycle, probe, none, false), Decision::Blocked);
        probe.set(Feature::Lifecycle, probed(CapabilityState::Unsupported, 501));
        QCOMPARE(capabilities::decide(Feature::Lifecycle, probe, none, false), Decision::Blocked);
    }

    void mergeIsAsymmetric()
    {
        CapabilitySet before;
        before.set(Feature::Cors, probed(CapabilityState::Supported));
        before.set(Feature::Website, probed(CapabilityState::Unsupported, 501));
        before.set(Feature::Lifecycle, probed(CapabilityState::Supported));
        before.set(Feature::Logging, probed(CapabilityState::Unknown, 503, 1));
        before.set(Feature::Replication, probed(CapabilityState::Supported));

        CapabilitySet fresh;
        fresh.set(Feature::Cors, probed(CapabilityState::Unsupported, 501));    // downgrade
        fresh.set(Feature::Website, probed(CapabilityState::Supported));        // validated upgrade
        fresh.set(Feature::Lifecycle, probed(CapabilityState::Unknown, 503, 1)); // transient
        fresh.set(Feature::Logging, probed(CapabilityState::Unknown, 503, 1));
        // Replication not probed this time.

        const CapabilitySet merged = capabilities::merge(before, fresh);
        QCOMPARE(merged.state(Feature::Cors), CapabilityState::Unsupported);
        QCOMPARE(merged.state(Feature::Website), CapabilityState::Supported);
        QCOMPARE(merged.state(Feature::Lifecycle), CapabilityState::Supported);   // a 5xx never erases it
        QCOMPARE(merged.entry(Feature::Logging).unknownAttempts, 2);              // back-off grows
        QCOMPARE(merged.state(Feature::Replication), CapabilityState::Supported);

        const QList<CapabilityChange> changes = capabilities::diff(before, merged);
        QCOMPARE(changes.size(), 2);
        for (const CapabilityChange& c : changes) {
            QVERIFY(c.feature == Feature::Cors || c.feature == Feature::Website);
            if (c.feature == Feature::Cors) {
                QCOMPARE(c.before, CapabilityState::Supported);
                QCOMPARE(c.after, CapabilityState::Unsupported);
            }
        }
    }

    void fingerprintChangeDiscardsOldAnswers()
    {
        CapabilitySet before;
        before.fingerprint.serverHeader = QStringLiteral("Ceph Object Gateway (squid)");
        before.set(Feature::Lifecycle, probed(CapabilityState::Supported));
        CapabilitySet fresh;
        fresh.fingerprint.serverHeader = QStringLiteral("SomethingElse/2.0");
        fresh.set(Feature::Lifecycle, probed(CapabilityState::Unknown, 503, 1));
        QCOMPARE(capabilities::merge(before, fresh).state(Feature::Lifecycle), CapabilityState::Unknown);
    }

    void expiry()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        CapabilitySet server;
        QVERIFY(capabilities::isExpired(server, now));   // never probed
        server.probedAt = now.addSecs(-3600);
        QVERIFY(!capabilities::isExpired(server, now));
        server.probedAt = now.addSecs(-qint64(25) * 3600);
        QVERIFY(capabilities::isExpired(server, now));

        CapabilitySet bucket;
        bucket.scope = CapabilityScope::Bucket;
        bucket.probedAt = now.addSecs(-qint64(5) * 60);
        QVERIFY(!capabilities::isExpired(bucket, now));
        bucket.probedAt = now.addSecs(-qint64(11) * 60);
        QVERIFY(capabilities::isExpired(bucket, now));

        // Unknown results come back sooner, with back-off: 5 min, 30 min, 2 h.
        server.probedAt = now.addSecs(-60);
        server.set(Feature::Cors, probed(CapabilityState::Unknown, 503, 1, now.addSecs(-qint64(6) * 60)));
        QVERIFY(capabilities::isExpired(server, now));
        server.set(Feature::Cors, probed(CapabilityState::Unknown, 503, 2, now.addSecs(-qint64(6) * 60)));
        QVERIFY(!capabilities::isExpired(server, now));
        server.set(Feature::Cors, probed(CapabilityState::Unknown, 503, 9, now.addSecs(-qint64(3) * 3600)));
        QVERIFY(capabilities::isExpired(server, now));
    }

    void jsonRoundTrip()
    {
        CapabilitySet set;
        set.scope = CapabilityScope::Bucket;
        set.bucket = QStringLiteral("bkt");
        set.probedAt = QDateTime(QDate(2026, 10, 20), QTime(8, 0), QTimeZone::UTC);
        set.fingerprint = {QStringLiteral("Ceph Object Gateway (squid)"),
                           {QStringLiteral("x-amz-request-id")}, QStringLiteral("rgw-tx")};
        set.bucketState.versioning = QStringLiteral("Enabled");
        set.bucketState.objectLockEnabled = true;
        set.bucketState.mfaDelete = false;
        CapabilityEntry e = probed(CapabilityState::Supported, 404, 0, set.probedAt);
        e.s3Code = QStringLiteral("NoSuchCORSConfiguration");
        set.set(Feature::Cors, e);
        set.set(Feature::Website, probed(CapabilityState::Unknown, 503, 2, set.probedAt));

        const Result<CapabilitySet> back = CapabilitySet::fromJson(set.toJson());
        QVERIFY(back);
        QCOMPARE(back->bucket, QStringLiteral("bkt"));
        QCOMPARE(back->scope, CapabilityScope::Bucket);
        QCOMPARE(back->probedAt, set.probedAt);
        QCOMPARE(back->fingerprint, set.fingerprint);
        QCOMPARE(back->bucketState.versioning, QStringLiteral("Enabled"));
        QCOMPARE(back->bucketState.objectLockEnabled, std::optional(true));
        QCOMPARE(back->bucketState.mfaDelete, std::optional(false));
        QCOMPARE(back->entry(Feature::Cors).s3Code, QStringLiteral("NoSuchCORSConfiguration"));
        QCOMPARE(back->entry(Feature::Cors).httpStatus, 404);
        QCOMPARE(back->entry(Feature::Website).unknownAttempts, 2);

        // Forward compatibility: ids from a newer version are ignored.
        QJsonObject json = set.toJson();
        QJsonObject features = json.value(QLatin1String("features")).toObject();
        features.insert(QStringLiteral("bucket.future-thing"), QJsonObject{{QStringLiteral("state"), QStringLiteral("Supported")}});
        json.insert(QStringLiteral("features"), features);
        QVERIFY(CapabilitySet::fromJson(json));

        json.insert(QStringLiteral("schema"), 99);
        QVERIFY(!CapabilitySet::fromJson(json));
    }
};

QTEST_GUILESS_MAIN(TestCapabilities)
#include "test_capabilities.moc"
