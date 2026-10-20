#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QtTest>

using namespace qbucket::s3;

class TestProviderGeneric : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void s3Defaults()
    {
        const GenericS3Profile p;
        QCOMPARE(p.id(), QStringLiteral("generic"));
        QVERIFY(p.regions().isEmpty());
        QVERIFY(!p.endpointFor(QStringLiteral("x")).isValid());   // user enters the endpoint
        QCOMPARE(p.defaultAddressing(), AddressingStyle::Path);
        QCOMPARE(p.signingRegion(QString()), QStringLiteral("us-east-1"));
        QCOMPARE(p.maxDeleteBatch(), 1000);
        QCOMPARE(p.maxKeysPerList(), 1000);
        QCOMPARE(p.maxParts(), 10000);
        QCOMPARE(p.listApi(), ListApi::V2);
    }

    void normalizesEtags()
    {
        ListPage page;
        page.objects = {{QStringLiteral("a"), 1, {}, QStringLiteral("abc"), {}},
                        {QStringLiteral("b"), 1, {}, QStringLiteral("\"def\""), {}},
                        {QStringLiteral("c"), 1, {}, QString(), {}}};
        GenericS3Profile().normalizeListing(page);
        QCOMPARE(page.objects[0].etag, QStringLiteral("\"abc\""));
        QCOMPARE(page.objects[1].etag, QStringLiteral("\"def\""));
        QVERIFY(page.objects[2].etag.isEmpty());
    }

    void registryLookup()
    {
        const ProviderRegistry& r = ProviderRegistry::builtin();
        QCOMPARE(r.findOrGeneric(QStringLiteral("does-not-exist"))->id(), QStringLiteral("generic"));
        QCOMPARE(r.findOrGeneric(QString())->id(), QStringLiteral("generic"));
        QVERIFY(!r.find(QStringLiteral("does-not-exist")));
        QStringList ids;
        for (const auto& p : r.profiles())
            ids.append(p->id());
        QCOMPARE(ids, (QStringList{QStringLiteral("hetzner"), QStringLiteral("aws"),
                                   QStringLiteral("garage"), QStringLiteral("generic")}));

        ProviderRegistry own;
        own.add(std::make_shared<GenericS3Profile>());
        own.add(std::make_shared<GenericS3Profile>());   // same id replaces
        QCOMPARE(own.profiles().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestProviderGeneric)
#include "test_provider_generic.moc"
