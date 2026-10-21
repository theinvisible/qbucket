#include "capabilities/ProbeInterpreter.h"
#include "core/ErrorMapping.h"

#include <qbucket/s3/providers/CephProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QFile>
#include <QtTest>

#include <algorithm>

using namespace qbucket::s3;

namespace {

QByteArray fixture(const char* name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/ceph/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", name);
    return f.readAll();
}

// The bucket probe of `f` (the first probe if there is none: the test's
// comparisons fail then).
const ProbeSpec& probeFor(Feature f)
{
    const QList<ProbeSpec>& probes = bucketProbes();
    const auto it = std::find_if(probes.cbegin(), probes.cend(), [f](const ProbeSpec& s) { return s.feature == f; });
    return it != probes.cend() ? *it : probes.front();
}

} // namespace

class TestProviderCeph : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void defaults()
    {
        const auto p = ProviderRegistry::builtin().find(QStringLiteral("ceph"));
        QVERIFY(p);
        QCOMPARE(p->defaultAddressing(), AddressingStyle::Path);
        QCOMPARE(p->storageClasses(), QStringList{QStringLiteral("STANDARD")});
        // Confirmed by it_s3_backend_ceph.
        for (const Feature f : {Feature::ConditionalWrites, Feature::ObjectTaggingWrite, Feature::BucketVersioningWrite,
                                Feature::BucketPolicyWrite, Feature::CorsWrite, Feature::LifecycleWrite,
                                Feature::ObjectLockWrite, Feature::BucketAclWrite, Feature::PublicAccessBlockWrite,
                                Feature::RetentionWrite, Feature::LegalHoldWrite})
            QCOMPARE(p->capabilities().state(f), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::SseC), CapabilityState::Unknown);
    }

    void probeAnswers()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        // No object lock on the bucket: supported, not configured.
        BucketState state;
        Error noLock = errorFromResponse(404, fixture("object-lock-not-configured.xml"));
        const CapabilityEntry lock = interpretProbe(probeFor(Feature::ObjectLock), Result<RawResponse>(noLock), now, &state);
        QCOMPARE(lock.state, CapabilityState::Supported);
        QCOMPARE(state.objectLockEnabled, std::optional(false));
        // ?ownershipControls ignored: the listing comes back.
        RawResponse listing;
        listing.status = 200;
        listing.body = fixture("ownership-controls-ignored.xml");
        const CapabilityEntry ownership =
            interpretProbe(probeFor(Feature::OwnershipControls), Result<RawResponse>(listing), now);
        QCOMPARE(ownership.state, CapabilityState::Unsupported);
        QCOMPARE(ownership.s3Code, QStringLiteral("Root:ListBucketResult"));
    }
};

QTEST_GUILESS_MAIN(TestProviderCeph)
#include "test_provider_ceph.moc"
