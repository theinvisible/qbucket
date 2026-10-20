#include <qbucket/s3/Result.h>
#include <qbucket/s3/Version.h>

#include <cstdio>

int main()
{
    const qbucket::s3::Result<int> r =
        qbucket::s3::Error::make(qbucket::s3::ErrorCategory::NotFound, QStringLiteral("probe"));
    std::printf("qbucket-s3 %s: %s\n", QBUCKET_S3_VERSION_STRING,
                qPrintable(r.error().toString()));
    return r ? 1 : 0;
}
