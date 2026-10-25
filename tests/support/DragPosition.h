#pragma once

#include <QPoint>
#include <QPointF>
#include <QtGlobal>

namespace qbucket::testing {

// The position type to build a QDragEnterEvent / QDragMoveEvent with. Qt 6.12
// added constructors taking QPointF and deprecated the QPoint ones (MSVC /WX
// fails on them); older Qt has only those.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
using DragPosition = QPointF;
#else
using DragPosition = QPoint;
#endif

} // namespace qbucket::testing
