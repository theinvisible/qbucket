#pragma once

#include "platform/ExternalDrag.h"

namespace qbucket {

// Dragging out with local copies (Linux, Flatpak, any platform without a
// virtual-file drag): a drag of a staged selection carries its files as
// URLs. Otherwise the drag still offers URLs, while a small selection is
// staged right away (DragStaging::prepare, quiet) — files are handed over
// only at the drop, when they are there; a drop that comes too early gets
// nothing and the user is told to drag again. Never blocks for a download.
class StagedDrag : public ExternalDrag {
    Q_OBJECT

public:
    explicit StagedDrag(DragStaging* staging, QObject* parent = nullptr);

    Qt::DropAction exec(QObject* source, QMimeData* internal, const DragStaging::Selection& selection,
                        Qt::DropActions actions, Qt::DropAction defaultAction) override;

private:
    DragStaging* m_staging;
};

} // namespace qbucket
