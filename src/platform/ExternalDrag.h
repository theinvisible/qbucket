#pragma once

#include "controllers/DragStaging.h"

#include <QObject>

#include <memory>

class QMimeData;

namespace qbucket {

class AccountSessions;
class OperationController;

// How objects leave the browser for a file manager (PLAN.md §6 M7). The
// browser hands over its in-app drag data (objects of this instance, for
// drops on folders and buckets) and the selection; the platform adds what a
// file manager can take and runs the drag:
//   - StagedDrag (Linux, Flatpak, fallback): local copies prepared by
//     DragStaging, offered as file URLs;
//   - VirtualFileDrag (Windows): files Explorer streams on the drop.
// Everything platform-specific stays in src/platform/.
class ExternalDrag : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ExternalDrag() override;

    // Runs the drag from `source` like QDrag::exec; `internal` (taken over)
    // carries the in-app formats.
    virtual Qt::DropAction exec(QObject* source, QMimeData* internal, const DragStaging::Selection& selection,
                                Qt::DropActions actions, Qt::DropAction defaultAction) = 0;

    // Local copies must be prepared first (StagedDrag): the browser then
    // offers "Prepare for Dragging Out".
    [[nodiscard]] virtual bool needsStaging() const { return true; }

    // The platform's way: VirtualFileDrag on Windows, StagedDrag elsewhere.
    static std::unique_ptr<ExternalDrag> create(DragStaging* staging, OperationController* operations,
                                                QObject* parent = nullptr);
};

} // namespace qbucket
