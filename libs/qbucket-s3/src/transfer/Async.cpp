#include "transfer/Async.h"

namespace qbucket::s3::transfer::detail {

QThreadPool* serialStorePool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;   // lives until exit; its thread expires when idle
        p->setMaxThreadCount(1);
        p->setObjectName(QStringLiteral("qbucket-s3 resume store"));
        return p;
    }();
    return pool;
}

} // namespace qbucket::s3::transfer::detail
