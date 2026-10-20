#include "capabilities/ProbeInterpreter.h"
#include "core/ErrorMapping.h"

#include <QFile>
#include <QtTest>

using namespace qbucket::s3;

namespace {

ProbeSpec specFor(Feature f)
{
    for (const ProbeSpec& p : bucketProbes() + objectProbes()) {
        if (p.feature == f)
            return p;
    }
    qFatal("no probe for %s", qPrintable(featureId(f)));
    Q_UNREACHABLE_RETURN(bucketProbes().first());
}

Result<RawResponse> ok(int status, const QByteArray& body)
{
    RawResponse r;
    r.status = status;
    r.body = body;
    return r;
}

Result<RawResponse> fail(int status, const QByteArray& code)
{
    return errorFromResponse(status, code.isEmpty() ? QByteArray()
                                                    : "<Error><Code>" + code + "</Code></Error>");
}

Result<RawResponse> transient()
{
    Error e = Error::make(ErrorCategory::Transient, QStringLiteral("timeout"));
    return e;
}

} // namespace

// The §3.3 response-interpretation table, row by row.
class TestProbeInterpreter : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void table_data()
    {
        QTest::addColumn<int>("feature");
        QTest::addColumn<int>("kind");   // 0 ok, 1 error, 2 transient
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<int>("state");
        QTest::addColumn<bool>("configured");

        const auto f = [](Feature x) { return int(x); };
        const int S = int(CapabilityState::Supported);
        const int U = int(CapabilityState::Unsupported);
        const int N = int(CapabilityState::NotPermitted);
        const int K = int(CapabilityState::Unknown);

        QTest::newRow("200 expected root, empty") << f(Feature::BucketVersioning) << 0 << 200
            << QByteArray("<VersioningConfiguration/>") << S << false;
        QTest::newRow("200 expected root, set") << f(Feature::BucketVersioning) << 0 << 200
            << QByteArray("<VersioningConfiguration><Status>Enabled</Status></VersioningConfiguration>") << S << true;
        QTest::newRow("200 lifecycle rules") << f(Feature::Lifecycle) << 0 << 200
            << QByteArray("<LifecycleConfiguration><Rule><ID>x</ID></Rule></LifecycleConfiguration>") << S << true;
        QTest::newRow("404 not configured") << f(Feature::Cors) << 1 << 404
            << QByteArray("NoSuchCORSConfiguration") << S << false;
        QTest::newRow("404 object lock not configured") << f(Feature::ObjectLock) << 1 << 404
            << QByteArray("ObjectLockConfigurationNotFoundError") << S << false;
        QTest::newRow("501 not implemented") << f(Feature::Website) << 1 << 501
            << QByteArray("NotImplemented") << U << false;
        QTest::newRow("405 on a probe") << f(Feature::Website) << 1 << 405
            << QByteArray("MethodNotAllowed") << U << false;
        QTest::newRow("ignored subresource (listing)") << f(Feature::OwnershipControls) << 0 << 200
            << QByteArray("<ListBucketResult><Name>b</Name></ListBucketResult>") << U << false;
        QTest::newRow("200 not xml") << f(Feature::Logging) << 0 << 200 << QByteArray("hello") << U << false;
        QTest::newRow("204 nothing set") << f(Feature::Website) << 0 << 204 << QByteArray() << S << false;
        QTest::newRow("403") << f(Feature::BucketAcl) << 1 << 403 << QByteArray("AccessDenied") << N << false;
        QTest::newRow("503 never unsupported") << f(Feature::Cors) << 1 << 503 << QByteArray("SlowDown") << K << false;
        QTest::newRow("500") << f(Feature::Cors) << 1 << 500 << QByteArray("InternalError") << K << false;
        QTest::newRow("timeout") << f(Feature::Cors) << 2 << 0 << QByteArray() << K << false;
        QTest::newRow("NoSuchBucket") << f(Feature::Cors) << 1 << 404 << QByteArray("NoSuchBucket") << K << false;
        QTest::newRow("odd 400") << f(Feature::Cors) << 1 << 400 << QByteArray("InvalidRequest") << K << false;
        QTest::newRow("policy json") << f(Feature::BucketPolicy) << 0 << 200
            << QByteArray("{\"Version\":\"2012-10-17\",\"Statement\":[]}") << S << true;
        QTest::newRow("policy ignored") << f(Feature::BucketPolicy) << 0 << 200
            << QByteArray("<ListBucketResult/>") << U << false;
        QTest::newRow("rgw empty replication") << f(Feature::Replication) << 0 << 200
            << QByteArray("<ReplicationConfiguration><Role></Role></ReplicationConfiguration>") << S << false;
        QTest::newRow("policy status is no config") << f(Feature::PolicyStatus) << 0 << 200
            << QByteArray("<PolicyStatus><IsPublic>false</IsPublic></PolicyStatus>") << S << false;
    }

    void table()
    {
        QFETCH(int, feature);
        QFETCH(int, kind);
        QFETCH(int, status);
        QFETCH(QByteArray, payload);
        QFETCH(int, state);
        QFETCH(bool, configured);
        const Result<RawResponse> r = kind == 0 ? ok(status, payload)
                                    : kind == 1 ? fail(status, payload)
                                                : transient();
        const CapabilityEntry e = interpretProbe(specFor(Feature(feature)), r, QDateTime::currentDateTimeUtc());
        QCOMPARE(stateName(e.state), stateName(CapabilityState(state)));
        QCOMPARE(e.configured, configured);
        QCOMPARE(e.source, CapabilitySource::Probed);
        QVERIFY(e.checkedAt.isValid());
        if (e.state == CapabilityState::Unknown)
            QCOMPARE(e.unknownAttempts, 1);
    }

    void bucketState()
    {
        BucketState s;
        (void)interpretProbe(specFor(Feature::BucketVersioning),
                             ok(200, "<VersioningConfiguration><Status>Suspended</Status></VersioningConfiguration>"),
                             QDateTime::currentDateTimeUtc(), &s);
        QCOMPARE(s.versioning, QStringLiteral("Suspended"));
        (void)interpretProbe(specFor(Feature::ObjectLock),
                             ok(200, "<ObjectLockConfiguration><ObjectLockEnabled>Enabled</ObjectLockEnabled></ObjectLockConfiguration>"),
                             QDateTime::currentDateTimeUtc(), &s);
        QCOMPARE(s.objectLockEnabled, std::optional(true));
        (void)interpretProbe(specFor(Feature::ObjectLock), fail(404, "ObjectLockConfigurationNotFoundError"),
                             QDateTime::currentDateTimeUtc(), &s);
        QCOMPARE(s.objectLockEnabled, std::optional(false));
    }

    void hetznerObjectLockFixtures()
    {
        const auto fixture = [](const char* name) {
            QFile f(QStringLiteral(TEST_DATA_DIR "/hetzner/") + QLatin1String(name));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        BucketState s;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        CapabilityEntry e = interpretProbe(specFor(Feature::ObjectLock), ok(200, fixture("lock-object-lock-200.xml")), now, &s);
        QCOMPARE(e.state, CapabilityState::Supported);
        QCOMPARE(s.objectLockEnabled, std::optional(true));
        e = interpretProbe(specFor(Feature::BucketVersioning), ok(200, fixture("lock-versioning-200.xml")), now, &s);
        QCOMPARE(s.versioning, QStringLiteral("Enabled"));
        e = interpretProbe(specFor(Feature::LegalHold),
                           errorFromResponse(404, fixture("lock-object-legal-hold-404.xml")), now, &s);
        QCOMPARE(e.state, CapabilityState::Supported);
        e = interpretProbe(specFor(Feature::Retention),
                           errorFromResponse(404, fixture("lock-object-retention-404.xml")), now, &s);
        QCOMPARE(e.state, CapabilityState::Supported);
    }

    void fingerprints()
    {
        RawResponse rgw;
        rgw.headers = {{"Server", "Ceph Object Gateway (squid)"},
                       {"x-amz-request-id", "tx00000c3b1f5e8a1d2c3b4-0067a1b2c3-1234-default"},
                       {"X-Amz-Bucket-Region", "default"}};
        const ServerFingerprint a = fingerprintOf(rgw);
        QCOMPARE(a.requestIdShape, QStringLiteral("rgw-tx"));
        QCOMPARE(a.amzHeaderNames, (QStringList{QStringLiteral("x-amz-bucket-region"), QStringLiteral("x-amz-request-id")}));

        RawResponse other = rgw;
        other.headers[1].second = "tx00000aaaaaaaaaaaaaaaa-0067a1b2c3-9999-default";
        QCOMPARE(fingerprintOf(other).hash(), a.hash());   // same software, other request

        RawResponse aws;
        aws.headers = {{"Server", "AmazonS3"}, {"x-amz-request-id", "4442587FB7D0A2F9"}};
        QCOMPARE(fingerprintOf(aws).requestIdShape, QStringLiteral("hex16"));
        QVERIFY(fingerprintOf(aws).hash() != a.hash());
    }
};

QTEST_GUILESS_MAIN(TestProbeInterpreter)
#include "test_probe_interpreter.moc"
