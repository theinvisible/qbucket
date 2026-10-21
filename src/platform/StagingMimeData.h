#pragma once

#include "controllers/DragStaging.h"

#include <QMimeData>
#include <QPointer>

#include <memory>

namespace qbucket {

// Drag data with the in-app formats of `internal` plus "text/uri-list"
// filled in only when a drop target asks for it: the staged copies of the
// selection if they are ready, nothing (and a note to the user) if not.
// Asked on the GUI thread at the drop; never waits.
class StagingMimeData : public QMimeData {
    Q_OBJECT

public:
    StagingMimeData(std::unique_ptr<QMimeData> internal, DragStaging* staging, DragStaging::Selection selection);
    ~StagingMimeData() override;

    [[nodiscard]] QStringList formats() const override;
    [[nodiscard]] bool hasFormat(const QString& mimeType) const override;

protected:
    [[nodiscard]] QVariant retrieveData(const QString& mimeType, QMetaType type) const override;

private:
    std::unique_ptr<QMimeData> m_internal;
    QPointer<DragStaging> m_staging;
    DragStaging::Selection m_selection;
    mutable bool m_reported = false;
};

} // namespace qbucket
