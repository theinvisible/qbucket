// The typed bucket configuration documents (BucketConfig.h): parsing,
// building, round trips, losslessness, validation, semantic equality,
// policy findings and bucket name rules. Pure functions.

#include <qbucket/s3/BucketConfig.h>

#include <QFile>
#include <QtTest>

using namespace qbucket::s3;

namespace {

QByteArray fixture(const char* name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/bucket-config/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", name);
    return f.readAll();
}

} // namespace

class TestBucketConfig : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void kindsAreConsistent()
    {
        for (const BucketConfig kind : allBucketConfigs()) {
            QVERIFY(!bucketConfigSubresource(kind).isEmpty());
            QCOMPARE(featuresFor(bucketConfigGetOperation(kind)), QList<Feature>{bucketConfigFeature(kind)});
            // Writable kinds have a write-only feature, and their writes need it first.
            if (const std::optional<Operation> put = bucketConfigPutOperation(kind)) {
                QVERIFY(bucketConfigWriteFeature(kind));
                QVERIFY(isMutating(*put));
                QCOMPARE(featuresFor(*put).value(0), *bucketConfigWriteFeature(kind));
            } else {
                QVERIFY(!bucketConfigWriteFeature(kind));
                QVERIFY(!bucketConfigDeleteOperation(kind));
            }
            if (const std::optional<Operation> del = bucketConfigDeleteOperation(kind)) {
                QVERIFY(isMutating(*del));
                QCOMPARE(featuresFor(*del).value(0), *bucketConfigWriteFeature(kind));
            }
            QVERIFY(!isMutating(bucketConfigGetOperation(kind)));
        }
        QCOMPARE(bucketConfigSubresource(BucketConfig::PublicAccessBlock), QByteArray("publicAccessBlock"));
        QVERIFY(bucketConfigRoot(BucketConfig::Policy).isEmpty());
        QVERIFY(!bucketConfigPutOperation(BucketConfig::Website));
        QVERIFY(!bucketConfigDeleteOperation(BucketConfig::ObjectLock));   // removing the default is a PUT
    }

    void cors()
    {
        const Result<CorsConfiguration> c = parseCorsConfiguration(fixture("cors.xml"));
        QVERIFY(c);
        QVERIFY(c->lossless);
        QCOMPARE(c->rules.size(), 2);
        const CorsRule& web = c->rules[0];
        QCOMPARE(web.id, QStringLiteral("web"));
        QCOMPARE(web.allowedOrigins, (QStringList{QStringLiteral("https://www.example.com"),
                                                  QStringLiteral("https://*.example.org")}));
        QCOMPARE(web.allowedMethods, (QStringList{QStringLiteral("GET"), QStringLiteral("PUT")}));
        QCOMPARE(web.exposeHeaders.size(), 2);
        QCOMPARE(web.maxAgeSeconds, std::optional(3000));
        QVERIFY(!c->rules[1].maxAgeSeconds);
        QVERIFY(corsProblem(*c).isEmpty());

        const Result<QByteArray> built = buildCorsConfiguration(*c);
        QVERIFY(built);
        QVERIFY(equivalentBucketConfig(BucketConfig::Cors, fixture("cors.xml"), *built));
        const Result<CorsConfiguration> again = parseCorsConfiguration(*built);
        QVERIFY(again);
        QCOMPARE(again->rules, c->rules);

        CorsConfiguration bad = *c;
        bad.rules[1].allowedMethods = {QStringLiteral("PATCH")};
        QVERIFY(!corsProblem(bad).isEmpty());
        bad = *c;
        bad.rules[0].allowedOrigins = {QStringLiteral("https://*.*.example.com")};
        QVERIFY(!corsProblem(bad).isEmpty());
        QVERIFY(!corsProblem(CorsConfiguration{}).isEmpty());
        bad = *c;
        bad.rules[0].id = QStringLiteral("bell\x07");
        QVERIFY(!buildCorsConfiguration(bad));

        // A rule element the type doesn't know: not lossless.
        QByteArray extra = fixture("cors.xml");
        extra.replace("<MaxAgeSeconds>", "<Fancy>1</Fancy><MaxAgeSeconds>");
        const Result<CorsConfiguration> lossy = parseCorsConfiguration(extra);
        QVERIFY(lossy);
        QVERIFY(!lossy->lossless);
        QVERIFY(!parseCorsConfiguration("<LifecycleConfiguration/>"));
    }

    void lifecycle()
    {
        const Result<LifecycleConfiguration> c = parseLifecycleConfiguration(fixture("lifecycle.xml"));
        QVERIFY(c);
        QVERIFY(c->lossless);
        QCOMPARE(c->rules.size(), 3);
        const LifecycleRule& logs = c->rules[0];
        QCOMPARE(logs.prefix, QStringLiteral("logs/"));
        QCOMPARE(logs.tags, (TagSet{{QStringLiteral("keep"), QStringLiteral("no")},
                                    {QStringLiteral("team"), QStringLiteral("ops & dev")}}));
        QVERIFY(logs.enabled);
        QCOMPARE(logs.transitions.size(), 1);
        QCOMPARE(logs.transitions[0].days, std::optional(30));
        QCOMPARE(logs.transitions[0].storageClass, QStringLiteral("STANDARD_IA"));
        QCOMPARE(logs.expirationDays, std::optional(365));
        QCOMPARE(logs.noncurrentDays, std::optional(7));
        QCOMPARE(logs.newerNoncurrentVersions, std::optional(3));
        const LifecycleRule& uploads = c->rules[1];
        QVERIFY(!uploads.enabled);
        QVERIFY(uploads.prefix.isEmpty());
        QVERIFY(uploads.expiredObjectDeleteMarker);
        QCOMPARE(uploads.abortIncompleteDays, std::optional(2));
        const LifecycleRule& tagged = c->rules[2];
        QCOMPARE(tagged.tags.size(), 1);
        QCOMPARE(tagged.expirationDate, QDate(2027, 1, 1));
        QVERIFY(lifecycleProblem(*c).isEmpty());

        const Result<QByteArray> built = buildLifecycleConfiguration(*c);
        QVERIFY(built);
        QVERIFY(built->contains("<Filter><Tag><Key>tmp</Key>"));      // one tag, no prefix: no <And>
        QVERIFY(built->contains("<Date>2027-01-01T00:00:00.000Z</Date>"));
        QVERIFY(equivalentBucketConfig(BucketConfig::Lifecycle, fixture("lifecycle.xml"), *built));

        // The legacy form stays legacy, and reads like the <Filter> form.
        const Result<LifecycleConfiguration> legacy = parseLifecycleConfiguration(fixture("lifecycle-legacy.xml"));
        QVERIFY(legacy);
        QVERIFY(legacy->lossless);
        QVERIFY(legacy->rules[0].legacyPrefix);
        QCOMPARE(legacy->rules[0].prefix, QStringLiteral("tmp/"));
        const Result<QByteArray> legacyBuilt = buildLifecycleConfiguration(*legacy);
        QVERIFY(legacyBuilt->contains("<ID>old-style</ID><Prefix>tmp/</Prefix><Status>"));
        LifecycleConfiguration modern = *legacy;
        modern.rules[0].legacyPrefix = false;
        QVERIFY(equivalentBucketConfig(BucketConfig::Lifecycle, *legacyBuilt, *buildLifecycleConfiguration(modern)));

        // What the types don't hold.
        const Result<LifecycleConfiguration> unknown = parseLifecycleConfiguration(fixture("lifecycle-unknown.xml"));
        QVERIFY(unknown);
        QVERIFY(!unknown->lossless);
    }

    void lifecycleReadBackWithServerIds()
    {
        LifecycleConfiguration mine;
        LifecycleRule rule;
        rule.prefix = QStringLiteral("tmp/");
        rule.expirationDays = 1;
        mine.rules = {rule};
        const QByteArray sent = *buildLifecycleConfiguration(mine);
        LifecycleConfiguration theirs = mine;
        theirs.rules[0].id = QStringLiteral("server-made-1");
        QVERIFY(equivalentBucketConfig(BucketConfig::Lifecycle, sent, *buildLifecycleConfiguration(theirs)));
        theirs.rules[0].expirationDays = 2;
        QVERIFY(!equivalentBucketConfig(BucketConfig::Lifecycle, sent, *buildLifecycleConfiguration(theirs)));
        // Our own ID must come back as sent.
        mine.rules[0].id = QStringLiteral("mine");
        theirs = mine;
        theirs.rules[0].id = QStringLiteral("other");
        QVERIFY(!equivalentBucketConfig(BucketConfig::Lifecycle, *buildLifecycleConfiguration(mine),
                                        *buildLifecycleConfiguration(theirs)));
    }

    void lifecycleProblems()
    {
        LifecycleRule rule;
        LifecycleConfiguration c;
        c.rules = {rule};
        QVERIFY(!lifecycleProblem(c).isEmpty());   // no action
        c.rules[0].expirationDays = 0;
        QVERIFY(!lifecycleProblem(c).isEmpty());
        c.rules[0].expirationDays = 1;
        QVERIFY(lifecycleProblem(c).isEmpty());
        c.rules[0].expirationDate = QDate(2030, 1, 1);
        QVERIFY(!lifecycleProblem(c).isEmpty());   // days and date
        c.rules[0].expirationDate = {};
        c.rules[0].newerNoncurrentVersions = 2;
        QVERIFY(!lifecycleProblem(c).isEmpty());   // needs noncurrent days
        c.rules[0].noncurrentDays = 3;
        QVERIFY(lifecycleProblem(c).isEmpty());
        c.rules[0].tags = {{QStringLiteral("k"), QStringLiteral("v")}};
        c.rules[0].abortIncompleteDays = 1;
        QVERIFY(!lifecycleProblem(c).isEmpty());   // abort uploads + tags
        c.rules[0].abortIncompleteDays.reset();
        c.rules.append(c.rules[0]);
        c.rules[0].id = c.rules[1].id = QStringLiteral("same");
        QVERIFY(!lifecycleProblem(c).isEmpty());
    }

    void objectLock()
    {
        const Result<ObjectLockConfiguration> c = parseObjectLockConfiguration(fixture("object-lock.xml"));
        QVERIFY(c);
        QVERIFY(c->lossless);
        QVERIFY(c->enabled);
        QCOMPARE(c->mode, std::optional(ObjectLockMode::Governance));
        QCOMPARE(c->days, std::optional(3));
        QVERIFY(objectLockProblem(*c).isEmpty());
        QVERIFY(equivalentBucketConfig(BucketConfig::ObjectLock, fixture("object-lock.xml"),
                                       buildObjectLockConfiguration(*c)));
        ObjectLockConfiguration none = *c;
        none.mode.reset();
        none.days.reset();
        QVERIFY(objectLockProblem(none).isEmpty());   // no default retention
        QVERIFY(!buildObjectLockConfiguration(none).contains("<Rule>"));
        none.days = 1;
        QVERIFY(!objectLockProblem(none).isEmpty());
        ObjectLockConfiguration both = *c;
        both.years = 1;
        QVERIFY(!objectLockProblem(both).isEmpty());
        ObjectLockConfiguration off;
        QVERIFY(!objectLockProblem(off).isEmpty());   // only at creation
        const Result<ObjectLockConfiguration> enabledOnly =
            parseObjectLockConfiguration("<ObjectLockConfiguration><ObjectLockEnabled>Enabled</ObjectLockEnabled>"
                                         "</ObjectLockConfiguration>");
        QVERIFY(enabledOnly && enabledOnly->enabled && !enabledOnly->mode);
    }

    void publicAccessBlockAndStatus()
    {
        const Result<PublicAccessBlockConfiguration> c = parsePublicAccessBlock(fixture("public-access-block.xml"));
        QVERIFY(c);
        QVERIFY(c->lossless);
        QVERIFY(c->blockPublicAcls);
        QVERIFY(!c->ignorePublicAcls);
        QVERIFY(c->blockPublicPolicy);
        QVERIFY(!c->restrictPublicBuckets);
        QVERIFY(equivalentBucketConfig(BucketConfig::PublicAccessBlock, fixture("public-access-block.xml"),
                                       buildPublicAccessBlock(*c)));
        const Result<bool> isPublic = parsePolicyStatus("<PolicyStatus><IsPublic>true</IsPublic></PolicyStatus>");
        QVERIFY(isPublic && *isPublic);
        QVERIFY(!parsePolicyStatus("<Other/>"));
    }

    void policy()
    {
        const QByteArray json = fixture("policy-public-read.json");
        QVERIFY(policyProblems(json).isEmpty());
        const PolicyFindings f = analyzePolicy(json);
        QVERIFY(f.publicRead);
        QVERIFY(!f.publicWrite);
        QVERIFY(f.deny);
        // Reordered, compacted, single values as arrays and back: the same policy.
        QVERIFY(equivalentBucketConfig(BucketConfig::Policy, json, fixture("policy-reformatted.json")));
        QByteArray changed = fixture("policy-reformatted.json");
        changed.replace("s3:GetObject", "s3:PutObject");
        QVERIFY(!equivalentBucketConfig(BucketConfig::Policy, json, changed));
        QVERIFY(analyzePolicy(changed).publicWrite);

        QVERIFY(!policyProblems("{").isEmpty());
        QVERIFY(!policyProblems("[]").isEmpty());
        QVERIFY(!policyProblems(R"({"Version":"2012-10-17"})").isEmpty());
        const QStringList missing = policyProblems(R"({"Version":"2012-10-17","Statement":[{"Effect":"Maybe"}]})");
        QCOMPARE(missing.size(), 4);   // effect, action, resource, principal
    }

    void otherKindsCompareAsXml()
    {
        QVERIFY(equivalentBucketConfig(BucketConfig::Website,
                                       "<WebsiteConfiguration><IndexDocument><Suffix>index.html</Suffix>"
                                       "</IndexDocument></WebsiteConfiguration>",
                                       "<?xml version=\"1.0\"?>\n<WebsiteConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                                       "  <IndexDocument>\n    <Suffix>index.html</Suffix>\n  </IndexDocument>\n"
                                       "</WebsiteConfiguration>"));
        QVERIFY(!equivalentBucketConfig(BucketConfig::Website, "<WebsiteConfiguration/>",
                                        "<WebsiteConfiguration><IndexDocument/></WebsiteConfiguration>"));
    }

    void bucketNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("problem");
        const auto row = [](const char* name, BucketNameProblem p) {
            QTest::newRow(name) << QString::fromLatin1(name) << int(p);
        };
        row("qbucket-it-test", BucketNameProblem::None);
        row("my.dotted.bucket", BucketNameProblem::None);
        row("ab", BucketNameProblem::Length);
        row("Upper", BucketNameProblem::Characters);
        row("under_score", BucketNameProblem::Characters);
        row("-start", BucketNameProblem::StartOrEnd);
        row("end.", BucketNameProblem::StartOrEnd);
        row("two..dots", BucketNameProblem::AdjacentDots);
        row("dot.-dash", BucketNameProblem::DotNextToHyphen);
        row("192.168.5.4", BucketNameProblem::IpAddress);
        row("xn--punycode", BucketNameProblem::ReservedPrefix);
        row("access-s3alias", BucketNameProblem::ReservedSuffix);
        QTest::newRow("64 chars") << QString(64, QLatin1Char('a')) << int(BucketNameProblem::Length);
        QTest::newRow("63 chars") << QString(63, QLatin1Char('a')) << int(BucketNameProblem::None);
    }

    void bucketNames()
    {
        QFETCH(QString, name);
        QFETCH(int, problem);
        QCOMPARE(int(bucketNameProblem(name)), problem);
        QCOMPARE(bucketNameProblemText(bucketNameProblem(name)).isEmpty(), problem == int(BucketNameProblem::None));
    }
};

QTEST_GUILESS_MAIN(TestBucketConfig)
#include "test_bucket_config.moc"
