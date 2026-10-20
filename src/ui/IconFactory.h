#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace qbucket {

// ---------------------------------------------------------------------------
// IconFactory — line icons, ported from xyz-profiler.
// ---------------------------------------------------------------------------
// Same visual language as the design's `ICONS` path set: 24x24 viewBox, round
// caps/joins, 1.6 stroke. Each icon is rebuilt as an SVG document with the
// stroke colour injected, then rasterised via QSvgRenderer and cached in
// QPixmapCache. Colour comes from the active theme, so callers re-request
// icons on theme change to keep them tinted correctly.
// ---------------------------------------------------------------------------
namespace IconFactory {

// All known icon names (tests iterate this to make sure every icon renders).
QStringList names();

// Null pixmap / empty icon for an unknown name.
QPixmap pixmap(const QString& name, const QColor& color,
               int size = 18, qreal stroke = 1.6, qreal dpr = 1.0);

QIcon   icon(const QString& name, const QColor& color,
             int size = 18, qreal stroke = 1.6);

} // namespace IconFactory
} // namespace qbucket
