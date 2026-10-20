// Every public header, compiled with the library's full strictness
// (QT_NO_KEYWORDS, QT_NO_CAST_FROM_ASCII, …). The relaxed counterpart lives
// in public_headers_relaxed.cpp. Keep both include lists in sync.
#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Logging.h>
#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/RequestObserver.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/Version.h>

#include <QtTest>

int qbucketS3RelaxedHeadersCompiled();

class TestPublicHeaders : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void strictAndRelaxedBothCompile()
    {
        QCOMPARE(qbucketS3RelaxedHeadersCompiled(), 1);
    }

    void versionMacrosMatch()
    {
        const QString composed = QStringLiteral("%1.%2.%3")
                                     .arg(QBUCKET_S3_VERSION_MAJOR)
                                     .arg(QBUCKET_S3_VERSION_MINOR)
                                     .arg(QBUCKET_S3_VERSION_PATCH);
        QCOMPARE(composed, QStringLiteral(QBUCKET_S3_VERSION_STRING));
    }
};

QTEST_GUILESS_MAIN(TestPublicHeaders)
#include "test_public_headers.moc"
