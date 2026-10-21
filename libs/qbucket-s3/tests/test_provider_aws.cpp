#include <qbucket/s3/providers/AwsProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QtTest>

using namespace qbucket::s3;

class TestProviderAws : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void endpointsAndRegions()
    {
        const auto p = ProviderRegistry::builtin().find(QStringLiteral("aws"));
        QVERIFY(p);
        QVERIFY(p->regions().contains(QStringLiteral("eu-central-1")));
        QCOMPARE(p->endpointFor(QStringLiteral("eu-central-1")),
                 QUrl(QStringLiteral("https://s3.eu-central-1.amazonaws.com")));
        QCOMPARE(p->endpointFor(QString()), QUrl(QStringLiteral("https://s3.us-east-1.amazonaws.com")));
        QCOMPARE(p->defaultAddressing(), AddressingStyle::VirtualHosted);
        QCOMPARE(p->signingRegion(QString()), QStringLiteral("us-east-1"));
        QCOMPARE(p->capabilities().state(Feature::BatchDelete), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::CrossBucketCopy), CapabilityState::Supported);
        QVERIFY(p->storageClasses().contains(QStringLiteral("GLACIER_IR")));
        QCOMPARE(p->capabilities().state(Feature::ObjectTaggingWrite), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::ObjectAclWrite), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::BucketVersioningWrite), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::LifecycleWrite), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::LegalHoldWrite), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::ConditionalOverwrite), CapabilityState::Supported);
        QCOMPARE(p->cannedObjectAcls().size(), 6);
        QVERIFY(p->isArchiveStorageClass(QStringLiteral("DEEP_ARCHIVE")));
        QVERIFY(p->isArchiveStorageClass(QStringLiteral("glacier")));
        QVERIFY(!p->isArchiveStorageClass(QStringLiteral("GLACIER_IR")));   // instant retrieval
    }

    void legacyLocationConstraints()
    {
        const AwsProfile p;
        QCOMPARE(p.regionFromLocationConstraint(QString()), QStringLiteral("us-east-1"));
        QCOMPARE(p.regionFromLocationConstraint(QStringLiteral("EU")), QStringLiteral("eu-west-1"));
        QCOMPARE(p.locationConstraintFor(QStringLiteral("us-east-1")), QString());
        QCOMPARE(p.locationConstraintFor(QStringLiteral("eu-west-1")), QStringLiteral("eu-west-1"));
    }
};

QTEST_GUILESS_MAIN(TestProviderAws)
#include "test_provider_aws.moc"
