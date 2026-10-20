#include <qbucket/s3/RequestObserver.h>

namespace qbucket::s3 {

// Out-of-line so the vtable has one home (and one export) in the library.
RequestObserver::~RequestObserver() = default;

} // namespace qbucket::s3
