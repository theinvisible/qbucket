#include "platform/ExternalDrag.h"

#ifdef Q_OS_WIN
#include "platform/win/VirtualFileDrag.h"
#else
#include "platform/StagedDrag.h"
#endif

namespace qbucket {

ExternalDrag::~ExternalDrag() = default;

QString ExternalDrag::text(Text which)
{
    switch (which) {
    case Text::NotConnected: return tr("not connected");
    case Text::NoDragData: return tr("the drag data could not be created");
    case Text::CopyNotCompleted: return tr("the copy was not completed");
    }
    return {};
}

QString ExternalDrag::unavailable(const QString& why)
{
    return tr("Dragging out is not available: %1").arg(why);
}

QString ExternalDrag::failed(const QString& why)
{
    return tr("Dragging out failed: %1").arg(why);
}

QString ExternalDrag::done(int files, int verified)
{
    return tr("%n file(s) dragged out, %1 checked against their MD5.", nullptr, files).arg(verified);
}

std::unique_ptr<ExternalDrag> ExternalDrag::create(DragStaging* staging, OperationController* operations,
                                                   QObject* parent)
{
#ifdef Q_OS_WIN
    Q_UNUSED(staging);
    return std::make_unique<VirtualFileDrag>(operations, parent);
#else
    Q_UNUSED(operations);
    return std::make_unique<StagedDrag>(staging, parent);
#endif
}

} // namespace qbucket
