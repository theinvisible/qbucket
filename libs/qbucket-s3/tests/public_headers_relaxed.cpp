// Compiled WITHOUT the library's strictness defines (Qt keywords enabled,
// implicit ASCII casts allowed) — the way an arbitrary consumer includes us.
// Keep the include list in sync with test_public_headers.cpp.
#include <qbucket/s3/Capabilities.h>
#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Error.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/ListObjectsPager.h>
#include <qbucket/s3/Logging.h>
#include <qbucket/s3/Operation.h>
#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/RequestObserver.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Types.h>
#include <qbucket/s3/Version.h>
#include <qbucket/s3/providers/AwsProfile.h>
#include <qbucket/s3/providers/GarageProfile.h>
#include <qbucket/s3/providers/GenericS3Profile.h>
#include <qbucket/s3/providers/HetznerProfile.h>
#include <qbucket/s3/providers/ProviderProfile.h>
#include <qbucket/s3/providers/ProviderRegistry.h>
#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/Copier.h>
#include <qbucket/s3/transfer/Downloader.h>
#include <qbucket/s3/transfer/Export.h>
#include <qbucket/s3/transfer/Hasher.h>
#include <qbucket/s3/transfer/JsonFileResumeStore.h>
#include <qbucket/s3/transfer/PartPlanner.h>
#include <qbucket/s3/transfer/ResumeStore.h>
#include <qbucket/s3/transfer/Uploader.h>

int qbucketS3RelaxedHeadersCompiled()
{
    const qbucket::s3::Result<int> r = 1;
    return r.value();
}
