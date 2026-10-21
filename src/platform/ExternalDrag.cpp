#include "platform/ExternalDrag.h"

#include "platform/StagedDrag.h"

namespace qbucket {

ExternalDrag::~ExternalDrag() = default;

std::unique_ptr<ExternalDrag> ExternalDrag::create(DragStaging* staging, OperationController* operations,
                                                   QObject* parent)
{
    Q_UNUSED(operations);
    return std::make_unique<StagedDrag>(staging, parent);
}

} // namespace qbucket
