#include "core/ErrorMapping.h"
#include "http/S3Uri.h"

#include <qbucket/s3/providers/HetznerProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QFile>
#include <QtTest>

using namespace qbucket::s3;

class TestProviderHetzner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void identityAndRegions()
    {
        const auto p = ProviderRegistry::builtin().find(QStringLiteral("hetzner"));
        QVERIFY(p);
        QCOMPARE(p->displayName(), QStringLiteral("Hetzner Object Storage"));
        QCOMPARE(p->regions(),
                 (QStringList{QStringLiteral("fsn1"), QStringLiteral("nbg1"), QStringLiteral("hel1")}));
        QCOMPARE(p->storageClasses(), QStringList{QStringLiteral("STANDARD")});
        QCOMPARE(p->cannedObjectAcls(), (QStringList{QStringLiteral("private"), QStringLiteral("public-read")}));
        // Hetzner is the first, preselected preset.
        QCOMPARE(ProviderRegistry::builtin().profiles().first()->id(), QStringLiteral("hetzner"));
    }

    void endpointsAndSigning_data()
    {
        QTest::addColumn<QString>("region");
        QTest::newRow("fsn1") << QStringLiteral("fsn1");
        QTest::newRow("nbg1") << QStringLiteral("nbg1");
        QTest::newRow("hel1") << QStringLiteral("hel1");
    }

    void endpointsAndSigning()
    {
        QFETCH(QString, region);
        const HetznerProfile p;
        QCOMPARE(p.endpointFor(region), QUrl(QStringLiteral("https://%1.your-objectstorage.com").arg(region)));
        QCOMPARE(p.signingRegion(region), region);
        QCOMPARE(p.locationConstraintFor(region), region);
        QCOMPARE(p.regionFromLocationConstraint(region), region);
        QVERIFY(!p.endpointFor(QString()).isValid());
    }

    void sporadicAccessDeniedIsRetryable()
    {
        QFile f(QStringLiteral(TEST_DATA_DIR "/hetzner/error-access-denied-sporadic.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        Error e = errorFromResponse(403, f.readAll());
        HetznerProfile().mapError(e);
        QVERIFY(e.retryable);
        QCOMPARE(e.category, ErrorCategory::NotPermitted);   // stays a permission error

        Error head = errorFromResponse(403, QByteArray());   // HEAD: no body at all
        HetznerProfile().mapError(head);
        QVERIFY(head.retryable);

        Error explained = errorFromResponse(403, "<Error><Code>AccessDenied</Code>"
                                                 "<Message>Bucket policy denies it</Message></Error>");
        HetznerProfile().mapError(explained);
        QVERIFY(!explained.retryable);
        Error signature = errorFromResponse(403, "<Error><Code>SignatureDoesNotMatch</Code></Error>");
        HetznerProfile().mapError(signature);
        QVERIFY(!signature.retryable);
    }

    void encryptionReadProbeDoesNotUnlockWrites()
    {
        CapabilityEntry e{CapabilityState::Supported, CapabilitySource::Probed,
                          QDateTime::currentDateTimeUtc(), 404,
                          QStringLiteral("ServerSideEncryptionConfigurationNotFoundError"), false, 0};
        HetznerProfile().reviewProbe(Feature::BucketEncryption, e);
        QCOMPARE(e.state, CapabilityState::Unsupported);
        QCOMPARE(HetznerProfile().capabilities().state(Feature::BucketEncryption), CapabilityState::Unsupported);
        // Verified by writing (devlog 005), against the provider's own page.
        QCOMPARE(HetznerProfile().capabilities().state(Feature::CrossBucketCopy), CapabilityState::Supported);
        QCOMPARE(HetznerProfile().capabilities().state(Feature::BatchDelete), CapabilityState::Supported);
        // Verified on nbg1: If-None-Match is ignored (overwrites), multipart
        // requests keep their query string.
        QCOMPARE(HetznerProfile().capabilities().state(Feature::ConditionalWrites), CapabilityState::Unsupported);
        QCOMPARE(HetznerProfile().capabilities().state(Feature::MultipartUpload), CapabilityState::Supported);

        CapabilityEntry cors{CapabilityState::Supported, CapabilitySource::Probed, {}, 404, {}, false, 0};
        HetznerProfile().reviewProbe(Feature::Cors, cors);
        QCOMPARE(cors.state, CapabilityState::Supported);
    }

    void virtualHostedRequests()
    {
        const HetznerProfile p;
        Endpoint e;
        e.url = p.endpointFor(QStringLiteral("fsn1"));
        e.region = QStringLiteral("fsn1");
        const Result<S3Uri> uri = S3Uri::build(e, p.defaultAddressing(), QStringLiteral("my-bucket"),
                                               QStringLiteral("a b.txt"));
        QVERIFY(uri);
        QCOMPARE(uri->host(), QByteArray("my-bucket.fsn1.your-objectstorage.com"));
        QCOMPARE(uri->canonicalUri(), QByteArray("/a%20b.txt"));

        // Dotted names can't use the wildcard certificate: path-style.
        const Result<S3Uri> dotted = S3Uri::build(e, p.defaultAddressing(), QStringLiteral("my.bucket"),
                                                  QStringLiteral("k"));
        QVERIFY(dotted);
        QCOMPARE(dotted->host(), QByteArray("fsn1.your-objectstorage.com"));
    }
};

QTEST_GUILESS_MAIN(TestProviderHetzner)
#include "test_provider_hetzner.moc"
