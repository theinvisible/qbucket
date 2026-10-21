#pragma once

#include "platform/ExternalDrag.h"

#include <windows.h>
#include <objidl.h>

#include <functional>
#include <memory>

namespace qbucket {

class VirtualFileReader;

// Dragging out on Windows (PLAN.md §6 M7): Explorer gets virtual files —
// CFSTR_FILEDESCRIPTORW and a stream per file — and pulls the bytes only
// on the drop, asynchronously where it supports IDataObjectAsyncCapability.
// The data object is created in the process's MTA (kept alive with
// CoIncrementMTAUsage) and handed to DoDragDrop as a proxy, so neither the
// listing nor a download ever runs on the GUI thread. The in-app formats
// go along, registered under their MIME names as Qt's drop targets expect.
class VirtualFileDrag : public ExternalDrag {
    Q_OBJECT

public:
    explicit VirtualFileDrag(OperationController* operations, QObject* parent = nullptr);
    ~VirtualFileDrag() override;

    Qt::DropAction exec(QObject* source, QMimeData* internal, const DragStaging::Selection& selection,
                        Qt::DropActions actions, Qt::DropAction defaultAction) override;
    [[nodiscard]] bool needsStaging() const override { return false; }

    // The data object of a drag, created in the MTA and unmarshaled for the
    // calling (STA) thread; `finished` runs once when the drag and Explorer's
    // copy are over (on a COM thread). Null on failure. Public for its test.
    [[nodiscard]] static IDataObject* createDataObject(std::shared_ptr<VirtualFileReader> reader,
                                                       const QMimeData* internal,
                                                       std::function<void(HRESULT, DWORD)> finished);

private:
    OperationController* m_ops;
    CO_MTA_USAGE_COOKIE m_mta = nullptr;
};

} // namespace qbucket
