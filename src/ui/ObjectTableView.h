#pragma once

#include <QTableView>

#include <functional>

namespace qbucket {

// The object list's view: a QTableView whose drag start can be taken over
// (dragging out into a file manager, ExternalDrag). Without a starter, or
// when it declines, Qt's own drag runs.
class ObjectTableView : public QTableView {
    Q_OBJECT

public:
    using QTableView::QTableView;

    // Returns true when it ran the drag.
    void setDragStarter(std::function<bool(Qt::DropActions)> starter) { m_starter = std::move(starter); }
    // What a drag gesture starts (tests).
    void startDragForTesting(Qt::DropActions supportedActions) { startDrag(supportedActions); }

protected:
    void startDrag(Qt::DropActions supportedActions) override;

private:
    std::function<bool(Qt::DropActions)> m_starter;
};

} // namespace qbucket
