#include "ui/IconFactory.h"

#include <QByteArray>
#include <QHash>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace qbucket {
namespace IconFactory {
namespace {

// Generic icons are ported verbatim from xyz-profiler (which took them from
// the design's components.jsx ICONS map). The S3 / file-manager icons below
// them are new, drawn in the same style: 24x24, stroke only.
const QHash<QString, QString>& icons()
{
    static const QHash<QString, QString> kIcons = {
        // --- from xyz-profiler ---------------------------------------------
        {QStringLiteral("add"),      QStringLiteral("M12 5v14M5 12h14")},
        {QStringLiteral("edit"),     QStringLiteral("M4 20h4L18.5 9.5a2.1 2.1 0 0 0-3-3L5 17v3z M13.5 6.5l3 3")},
        {QStringLiteral("trash"),    QStringLiteral("M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13")},
        {QStringLiteral("search"),   QStringLiteral("M11 19a8 8 0 1 0 0-16 8 8 0 0 0 0 16zM21 21l-4.3-4.3")},
        {QStringLiteral("list"),     QStringLiteral("M8 6h13M8 12h13M8 18h13M3.5 6h.01M3.5 12h.01M3.5 18h.01")},
        {QStringLiteral("grid"),     QStringLiteral("M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z")},
        {QStringLiteral("sun"),      QStringLiteral("M12 4V2M12 22v-2M4 12H2M22 12h-2M5.6 5.6 4.2 4.2M19.8 19.8l-1.4-1.4M18.4 5.6l1.4-1.4M4.2 19.8l1.4-1.4M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z")},
        {QStringLiteral("moon"),     QStringLiteral("M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z")},
        {QStringLiteral("settings"), QStringLiteral("M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM19.4 13a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-2.9 1.2V21a2 2 0 1 1-4 0v-.1A1.7 1.7 0 0 0 7 19.4a1.7 1.7 0 0 0-1.9.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0-1.2-2.9H1a2 2 0 1 1 0-4h.1A1.7 1.7 0 0 0 2.6 7a1.7 1.7 0 0 0-.3-1.9l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.9.3H7a1.7 1.7 0 0 0 1-1.5V1a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.9-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.9V7a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z")},
        {QStringLiteral("chevron"),  QStringLiteral("M9 6l6 6-6 6")},
        {QStringLiteral("chevronLeft"), QStringLiteral("M15 6l-6 6 6 6")},
        {QStringLiteral("close"),    QStringLiteral("M6 6l12 12M18 6L6 18")},
        {QStringLiteral("sort"),     QStringLiteral("M7 4v16M7 4L4 7M7 4l3 3M17 20V4M17 20l3-3M17 20l-3-3")},
        {QStringLiteral("check"),    QStringLiteral("M5 12l5 5 9-10")},
        {QStringLiteral("filter"),   QStringLiteral("M3 5h18l-7 8v6l-4 2v-8z")},
        {QStringLiteral("refresh"),  QStringLiteral("M3 12a9 9 0 0 1 15-6.7L21 8M21 3v5h-5M21 12a9 9 0 0 1-15 6.7L3 16M3 21v-5h5")},
        {QStringLiteral("import"),   QStringLiteral("M12 3v11M8 10l4 4 4-4M5 20h14")},

        // --- QBucket: navigation ----------------------------------------
        {QStringLiteral("arrowLeft"),  QStringLiteral("M19 12H5M11 6l-6 6 6 6")},
        {QStringLiteral("arrowRight"), QStringLiteral("M5 12h14M13 6l6 6-6 6")},
        {QStringLiteral("arrowUp"),    QStringLiteral("M12 19V5M6 11l6-6 6 6")},

        // --- QBucket: storage objects -----------------------------------
        {QStringLiteral("bucket"),     QStringLiteral("M4 6.5c0-1.4 3.6-2.5 8-2.5s8 1.1 8 2.5-3.6 2.5-8 2.5-8-1.1-8-2.5zM4 6.5l1.8 12.2c.2 1.3 2.9 2.3 6.2 2.3s6-1 6.2-2.3L20 6.5")},
        {QStringLiteral("folder"),     QStringLiteral("M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h9A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z")},
        {QStringLiteral("folderOpen"), QStringLiteral("M3 18.5V6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h7A1.5 1.5 0 0 1 19 9v1.5M3 18.5l2.4-6.9A1.5 1.5 0 0 1 6.8 10.5h13.4a1 1 0 0 1 .9 1.3l-2.4 7.1a1.5 1.5 0 0 1-1.4 1.1H4.5A1.5 1.5 0 0 1 3 18.5z")},
        {QStringLiteral("folderPlus"), QStringLiteral("M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h9A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5zM12 11v6M9 14h6")},
        {QStringLiteral("file"),       QStringLiteral("M6 3h8l5 5v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zM14 3v5h5")},
        {QStringLiteral("server"),     QStringLiteral("M4 4h16v7H4zM4 13h16v7H4zM8 7.5h.01M8 16.5h.01")},
        {QStringLiteral("cloud"),      QStringLiteral("M7 18a4 4 0 0 1-.6-8A6 6 0 0 1 18 9a4.5 4.5 0 0 1-.5 9z")},

        // --- QBucket: transfers & object operations ---------------------
        {QStringLiteral("upload"),     QStringLiteral("M12 16V4M7 9l5-5 5 5M5 20h14")},
        {QStringLiteral("download"),   QStringLiteral("M12 4v12M7 11l5 5 5-5M5 20h14")},
        {QStringLiteral("pause"),      QStringLiteral("M8 5v14M16 5v14")},
        {QStringLiteral("copy"),       QStringLiteral("M9 9h11v11H9zM5 15H4V4h11v1")},
        {QStringLiteral("link"),       QStringLiteral("M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1")},
        {QStringLiteral("tag"),        QStringLiteral("M3 12V4a1 1 0 0 1 1-1h8l9 9-9 9zM7.5 7.5h.01")},
        {QStringLiteral("history"),    QStringLiteral("M3 12a9 9 0 1 0 2.6-6.4L3 8M3 3v5h5M12 7v5l3 2")},

        // --- QBucket: access & safety ----------------------------------
        {QStringLiteral("lock"),       QStringLiteral("M5 11h14v10H5zM8 11V7a4 4 0 0 1 8 0v4")},
        {QStringLiteral("unlock"),     QStringLiteral("M5 11h14v10H5zM8 11V7a4 4 0 0 1 7.6-1.7")},
        {QStringLiteral("key"),        QStringLiteral("M7.5 14a4.5 4.5 0 1 1 3.4-1.6L20 21.5M15.5 17l2-2M18 19.5l2-2")},
        {QStringLiteral("shield"),     QStringLiteral("M12 3l8 3v6c0 4.5-3.4 8.2-8 9-4.6-.8-8-4.5-8-9V6z")},
        {QStringLiteral("globe"),      QStringLiteral("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM3 12h18M12 3c2.5 2.5 3.8 5.5 3.8 9s-1.3 6.5-3.8 9c-2.5-2.5-3.8-5.5-3.8-9S9.5 5.5 12 3z")},
        {QStringLiteral("info"),       QStringLiteral("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 11v6M12 7.5h.01")},
        {QStringLiteral("warning"),    QStringLiteral("M12 3.5 2.5 20h19zM12 10v4.5M12 17.5h.01")},
    };
    return kIcons;
}

QByteArray buildSvg(const QString& name, const QColor& color, qreal stroke)
{
    const QString d = icons().value(name);
    if (d.isEmpty()) return {};

    // The design splits a single `d` string on "M" into separate <path>s.
    QString paths;
    const QStringList segs = d.split(QChar(u'M'), Qt::SkipEmptyParts);
    for (const QString& seg : segs)
        paths += QStringLiteral("<path d=\"M%1\"/>").arg(seg.trimmed());

    const QString svg = QStringLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
        "fill=\"none\" stroke=\"%1\" stroke-width=\"%2\" "
        "stroke-linecap=\"round\" stroke-linejoin=\"round\">%3</svg>")
        .arg(color.name(QColor::HexRgb))
        .arg(stroke)
        .arg(paths);

    return svg.toUtf8();
}

} // namespace

QStringList names()
{
    QStringList list = icons().keys();
    list.sort();
    return list;
}

QPixmap pixmap(const QString& name, const QColor& color,
               int size, qreal stroke, qreal dpr)
{
    // Rasterising an SVG on every call is wasteful (theme switches, row
    // painting). Cache by the full appearance key.
    const QString key = QStringLiteral("qbicon_%1_%2_%3_%4_%5")
        .arg(name, color.name(QColor::HexArgb), QString::number(size),
             QString::number(stroke), QString::number(dpr));
    QPixmap cached;
    if (QPixmapCache::find(key, &cached))
        return cached;

    const QByteArray svg = buildSvg(name, color, stroke);
    if (svg.isEmpty()) return {};

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};

    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();

    QPixmapCache::insert(key, pm);
    return pm;
}

QIcon icon(const QString& name, const QColor& color, int size, qreal stroke)
{
    const QPixmap pm1 = pixmap(name, color, size, stroke, 1.0);
    const QPixmap pm2 = pixmap(name, color, size, stroke, 2.0);
    QIcon ic;
    if (!pm1.isNull()) ic.addPixmap(pm1);
    if (!pm2.isNull()) ic.addPixmap(pm2);
    return ic;
}

} // namespace IconFactory
} // namespace qbucket
