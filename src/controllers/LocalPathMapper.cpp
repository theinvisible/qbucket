#include "controllers/LocalPathMapper.h"

#include <QCoreApplication>
#include <QStringList>

namespace qbucket {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qbucket::LocalPathMapper", text);
}

bool reservedOnWindows(const QString& segment)
{
    // The device name counts with any extension: "nul.txt" is NUL too.
    const QString base = segment.section(u'.', 0, 0).trimmed().toUpper();
    static const QStringList names = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                      QStringLiteral("NUL")};
    if (names.contains(base))
        return true;
    if (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT")))) {
        const QChar d = base.at(3);
        return d >= u'1' && d <= u'9';
    }
    return false;
}

} // namespace

LocalPathMapper::Rules LocalPathMapper::nativeRules()
{
#ifdef Q_OS_WIN
    return Rules::Windows;
#else
    return Rules::Posix;
#endif
}

QString LocalPathMapper::normalized(const QString& path, Rules rules)
{
    return rules == Rules::Windows ? path.toCaseFolded() : path;
}

LocalPathMapper::Result LocalPathMapper::map(const QString& relativeKey, Rules rules)
{
    Result r;
    if (relativeKey.isEmpty()) {
        r.reason = tr("empty name");
        return r;
    }
    const bool folder = relativeKey.endsWith(u'/');
    QStringList segments = relativeKey.split(u'/');
    if (folder)
        segments.removeLast();
    QStringList out;
    for (QString s : segments) {
        if (s == QLatin1String(".") || s == QLatin1String("..")) {
            r.reason = tr("the name contains “.” or “..” as a folder");
            return r;
        }
        const QString original = s;
        if (s.isEmpty())
            s = QStringLiteral("_");
        for (QChar& c : s) {
            const char16_t u = c.unicode();
            if (u == 0 || (rules == Rules::Windows && (u < 0x20 || QStringLiteral("<>:\"|?*\\").contains(c))))
                c = u'_';
        }
        if (rules == Rules::Windows) {
            while (!s.isEmpty() && (s.endsWith(u'.') || s.endsWith(u' ')))
                s[s.size() - 1] = u'_';
            if (reservedOnWindows(s))
                s.prepend(u'_');
        }
        if (s.toUtf8().size() > 255) {
            r.reason = tr("a part of the name is longer than 255 bytes");
            return r;
        }
        if (s != original)
            r.changed = true;
        out.append(s);
    }
    r.ok = true;
    r.path = out.join(u'/');
    if (folder)
        r.path += u'/';
    if (r.changed)
        r.reason = tr("characters this system does not allow were replaced");
    return r;
}

} // namespace qbucket
