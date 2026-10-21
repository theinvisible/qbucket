#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QtTest>

using namespace qbucket::s3;

class TestProviderRegistry : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void builtinOrder()
    {
        QStringList ids;
        for (const auto& p : ProviderRegistry::builtin().profiles())
            ids.append(p->id());
        QCOMPARE(ids, (QStringList{QStringLiteral("hetzner"), QStringLiteral("aws"), QStringLiteral("garage"),
                                   QStringLiteral("ceph"), QStringLiteral("generic")}));
    }

    void matchesPresetEndpointsExactly()
    {
        const ProviderRegistry& r = ProviderRegistry::builtin();
        auto m = r.matchEndpoint(QUrl(QStringLiteral("https://nbg1.your-objectstorage.com")));
        QVERIFY(m);
        QCOMPARE(m->profileId, QStringLiteral("hetzner"));
        QCOMPARE(m->region, QStringLiteral("nbg1"));

        // Case, a trailing slash and the default port make no difference.
        m = r.matchEndpoint(QUrl(QStringLiteral("HTTPS://FSN1.Your-ObjectStorage.com:443/")));
        QVERIFY(m);
        QCOMPARE(m->region, QStringLiteral("fsn1"));

        m = r.matchEndpoint(QUrl(QStringLiteral("https://s3.eu-central-1.amazonaws.com")));
        QVERIFY(m);
        QCOMPARE(m->profileId, QStringLiteral("aws"));
        QCOMPARE(m->region, QStringLiteral("eu-central-1"));
    }

    void anythingElseIsNoMatch()
    {
        const ProviderRegistry& r = ProviderRegistry::builtin();
        for (const char* url : {"https://nbg1.your-objectstorage.com/bucket", "http://nbg1.your-objectstorage.com",
                                "https://nbg1.your-objectstorage.com:8443", "https://bucket.nbg1.your-objectstorage.com",
                                "https://nbg1.your-objectstorage.com?x=1", "https://user@nbg1.your-objectstorage.com",
                                "https://evil-your-objectstorage.com", "http://localhost:9000", "", "not a url"}) {
            QVERIFY2(!r.matchEndpoint(QUrl(QString::fromLatin1(url))), url);
        }
    }
};

QTEST_GUILESS_MAIN(TestProviderRegistry)
#include "test_provider_registry.moc"
