#include "ui/Theme.h"

#include <QSet>

namespace qbucket {
namespace {

bool g_dark = true;

// Pre-computed colour-mix() results from theme.css (blue accent #2563EB).
// Identical to xyz-profiler's Theme.cpp — keep them in sync.
const Palette kLight = {
    /* accent       */ QColor(0x25, 0x63, 0xEB),
    /* accentFg     */ QColor(0xff, 0xff, 0xff),
    /* accentHover  */ QColor(0x21, 0x57, 0xCF),
    /* bg           */ QColor(0xc5, 0xc8, 0xcf),
    /* win          */ QColor(0xf3, 0xf4, 0xf6),
    /* panel        */ QColor(0xff, 0xff, 0xff),
    /* panel2       */ QColor(0xf7, 0xf8, 0xfa),
    /* panel3       */ QColor(0xee, 0xf0, 0xf3),
    /* border       */ QColor(0xe4, 0xe6, 0xea),
    /* borderStrong */ QColor(0xd3, 0xd6, 0xdc),
    /* text         */ QColor(0x1a, 0x1c, 0x20),
    /* text2        */ QColor(0x65, 0x6a, 0x73),
    /* text3        */ QColor(0x9a, 0xa0, 0xa9),
    /* hover        */ QColor(0xee, 0xf0, 0xf3),
    /* sel          */ QColor(0xe3, 0xeb, 0xfc),
    /* selLine      */ QColor(0xa8, 0xc1, 0xf7),
    /* titlebar     */ QColor(0xe9, 0xeb, 0xee),
    /* inset        */ QColor(0xff, 0xff, 0xff),
};

const Palette kDark = {
    /* accent       */ QColor(0x25, 0x63, 0xEB),
    /* accentFg     */ QColor(0xff, 0xff, 0xff),
    /* accentHover  */ QColor(0x21, 0x57, 0xCF),
    /* bg           */ QColor(0x13, 0x14, 0x18),
    /* win          */ QColor(0x1f, 0x21, 0x26),
    /* panel        */ QColor(0x26, 0x28, 0x2e),
    /* panel2       */ QColor(0x2b, 0x2e, 0x35),
    /* panel3       */ QColor(0x31, 0x34, 0x3c),
    /* border       */ QColor(0x34, 0x37, 0x3f),
    /* borderStrong */ QColor(0x41, 0x45, 0x4e),
    /* text         */ QColor(0xe7, 0xe9, 0xed),
    /* text2        */ QColor(0x9a, 0xa0, 0xab),
    /* text3        */ QColor(0x6c, 0x72, 0x7c),
    /* hover        */ QColor(0x2f, 0x32, 0x3a),
    /* sel          */ QColor(0x24, 0x36, 0x5e),
    /* selLine      */ QColor(0x25, 0x48, 0x95),
    /* titlebar     */ QColor(0x29, 0x2c, 0x32),
    /* inset        */ QColor(0x1c, 0x1e, 0x23),
};

} // namespace

namespace Theme {

bool isDark() { return g_dark; }
void setDark(bool dark) { g_dark = dark; }

const Palette& palette(bool dark) { return dark ? kDark : kLight; }

StorageClassBadge storageClassBadge(const QString& storageClass)
{
    const QString sc = storageClass.trimmed().toUpper();

    // S3 omits the storage class for STANDARD objects in some responses.
    if (sc.isEmpty() || sc == QLatin1String("STANDARD"))
        return { QStringLiteral("Standard"), current().text2, false };
    if (sc == QLatin1String("STANDARD_IA"))
        return { QStringLiteral("IA"), QColor(0x1f, 0x9a, 0x9a), true };
    if (sc == QLatin1String("ONEZONE_IA"))
        return { QStringLiteral("One Zone IA"), QColor(0x1f, 0x9a, 0x9a), true };
    if (sc == QLatin1String("INTELLIGENT_TIERING"))
        return { QStringLiteral("Int. Tiering"), QColor(0x2f, 0x6f, 0xd0), true };
    if (sc == QLatin1String("GLACIER") || sc == QLatin1String("GLACIER_IR")
        || sc == QLatin1String("DEEP_ARCHIVE"))
        return { sc == QLatin1String("DEEP_ARCHIVE") ? QStringLiteral("Deep Archive")
                                                     : QStringLiteral("Glacier"),
                 QColor(0x7c, 0x5c, 0xd6), true };
    if (sc == QLatin1String("REDUCED_REDUNDANCY"))
        return { QStringLiteral("Reduced"), QColor(0xb0, 0x7d, 0x18), true };

    // Provider-specific class — keep the server's label, neutral styling.
    return { storageClass.trimmed(), current().text2, false };
}

QColor fileTypeColor(const QString& extension)
{
    static const QSet<QString> archives = {
        QStringLiteral("zip"), QStringLiteral("tar"), QStringLiteral("gz"),
        QStringLiteral("tgz"), QStringLiteral("bz2"), QStringLiteral("xz"),
        QStringLiteral("zst"), QStringLiteral("7z"),  QStringLiteral("rar"),
        QStringLiteral("img"), QStringLiteral("iso"), QStringLiteral("vma"),
    };
    static const QSet<QString> images = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("svg"),
        QStringLiteral("bmp"), QStringLiteral("tif"),  QStringLiteral("tiff"),
        QStringLiteral("avif"), QStringLiteral("heic"),
    };
    static const QSet<QString> media = {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
        QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("mp3"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("m4a"),
    };
    static const QSet<QString> text = {
        QStringLiteral("txt"), QStringLiteral("md"),   QStringLiteral("json"),
        QStringLiteral("xml"), QStringLiteral("yml"),  QStringLiteral("yaml"),
        QStringLiteral("csv"), QStringLiteral("log"),  QStringLiteral("html"),
        QStringLiteral("css"), QStringLiteral("js"),   QStringLiteral("ts"),
        QStringLiteral("py"),  QStringLiteral("sh"),   QStringLiteral("cpp"),
        QStringLiteral("h"),   QStringLiteral("go"),   QStringLiteral("rs"),
        QStringLiteral("toml"), QStringLiteral("ini"), QStringLiteral("conf"),
    };

    const QString ext = extension.toLower();
    if (archives.contains(ext)) return QColor(0xb0, 0x7d, 0x18);
    if (images.contains(ext))   return QColor(0x2f, 0x9e, 0x5b);
    if (media.contains(ext))    return QColor(0x7c, 0x5c, 0xd6);
    if (text.contains(ext))     return QColor(0x2f, 0x6f, 0xd0);
    return current().text2;
}

} // namespace Theme
} // namespace qbucket
