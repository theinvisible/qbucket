#include "ui/ObjectTableView.h"

namespace qbucket {

void ObjectTableView::startDrag(Qt::DropActions supportedActions)
{
    if (m_starter && m_starter(supportedActions))
        return;
    QTableView::startDrag(supportedActions);
}

} // namespace qbucket
