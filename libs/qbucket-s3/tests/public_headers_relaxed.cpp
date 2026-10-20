// Compiled WITHOUT the library's strictness defines (Qt keywords enabled,
// implicit ASCII casts allowed) — the way an arbitrary consumer includes us.
// Keep the include list in sync with test_public_headers.cpp.
#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/Version.h>

int qbucketS3RelaxedHeadersCompiled()
{
    const qbucket::s3::Result<int> r = 1;
    return r.value();
}
