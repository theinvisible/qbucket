#include "http/S3Uri.h"

#include <qbucket/s3/providers/HetznerProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

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
