#include "platform/StagingMimeData.h"

#include <QUrl>

namespace qbucket {

namespace {
const QString kUriList = QStringLiteral("text/uri-list");
} // namespace

StagingMimeData::StagingMimeData(std::unique_ptr<QMimeData> internal, DragStaging* staging,
                                 DragStaging::Selection selection)
    : m_internal(std::move(internal)),
      m_staging(staging),
      m_selection(std::move(selection))
{
}

StagingMimeData::~StagingMimeData() = default;

QStringList StagingMimeData::formats() const
{
    QStringList out = m_internal ? m_internal->formats() : QStringList();
    if (!out.contains(kUriList))
        out.append(kUriList);
    return out;
}

bool StagingMimeData::hasFormat(const QString& mimeType) const
{
    return formats().contains(mimeType);
}

QVariant StagingMimeData::retrieveData(const QString& mimeType, QMetaType type) const
{
    if (mimeType != kUriList)
        return m_internal ? QVariant(m_internal->data(mimeType)) : QVariant();
    const QStringList paths = m_staging ? m_staging->readyPaths(m_selection) : QStringList();
    if (paths.isEmpty()) {
        // Asked once per drop (sometimes more): one note is enough.
        if (m_staging && !m_reported) {
            m_reported = true;
            m_staging->reportNotReady(m_selection);
        }
        return {};
    }
    QVariantList urls;
    for (const QString& p : paths)
        urls.append(QUrl::fromLocalFile(p));
    Q_UNUSED(type);
    return urls;
}

} // namespace qbucket
