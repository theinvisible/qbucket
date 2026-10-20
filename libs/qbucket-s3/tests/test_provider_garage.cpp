#include "core/ErrorMapping.h"

#include <qbucket/s3/providers/GarageProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QFile>
#include <QtTest>

using namespace qbucket::s3;

class TestProviderGarage : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void defaults()
    {
        const auto p = ProviderRegistry::builtin().find(QStringLiteral("garage"));
        QVERIFY(p);
        QCOMPARE(p->defaultAddressing(), AddressingStyle::Path);
        QCOMPARE(p->signingRegion(QString()), QStringLiteral("garage"));
        QCOMPARE(p->capabilities().state(Feature::BatchDelete), CapabilityState::Supported);
        // Ignores If-None-Match and overwrites (it_s3_backend_garage).
        QCOMPARE(p->capabilities().state(Feature::ConditionalWrites), CapabilityState::Unsupported);
        QCOMPARE(p->capabilities().state(Feature::MultipartUpload), CapabilityState::Supported);
        QCOMPARE(p->capabilities().state(Feature::BucketVersioning), CapabilityState::Unsupported);
    }

    void invalidSignatureIsAuthentication()
    {
        QFile f(QStringLiteral(TEST_DATA_DIR "/garage/error-invalid-signature.xml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        Error e = errorFromResponse(403, f.readAll());
        QCOMPARE(e.category, ErrorCategory::NotPermitted);   // generic reading
        GarageProfile().mapError(e);
        QCOMPARE(e.category, ErrorCategory::Authentication);

        // A real permission problem stays one.
        Error denied = errorFromResponse(403, "<Error><Code>AccessDenied</Code><Message>Forbidden: "
                                              "No such key</Message></Error>");
        GarageProfile().mapError(denied);
        QCOMPARE(denied.category, ErrorCategory::NotPermitted);
    }
};

QTEST_GUILESS_MAIN(TestProviderGarage)
#include "test_provider_garage.moc"
