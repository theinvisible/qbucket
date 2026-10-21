#include "platform/StagedDrag.h"

#include "platform/StagingMimeData.h"

#include <QDrag>

namespace qbucket {

StagedDrag::StagedDrag(DragStaging* staging, QObject* parent)
    : ExternalDrag(parent),
      m_staging(staging)
{
}

Qt::DropAction StagedDrag::exec(QObject* source, QMimeData* internal, const DragStaging::Selection& selection,
                                Qt::DropActions actions, Qt::DropAction defaultAction)
{
    // A small selection is fetched while the drag runs; the drop takes the
    // files if they are there by then.
    m_staging->prepare(selection, true);
    auto* drag = new QDrag(source);
    drag->setMimeData(new StagingMimeData(std::unique_ptr<QMimeData>(internal), m_staging, selection));
    // A file manager copies (it may offer "move", which takes our copy away).
    return drag->exec(actions | Qt::CopyAction, defaultAction);
}

} // namespace qbucket
