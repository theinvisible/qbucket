#include "core/HttpDate.h"

#include <QLocale>
#include <QTimeZone>

namespace qbucket::s3 {

QDateTime parseHttpDate(const QByteArray& value)
{
    const QString text = QString::fromLatin1(value).trimmed();
    QDateTime dt = QLocale::c().toDateTime(text, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!dt.isValid())
        dt = QDateTime::fromString(text, Qt::RFC2822Date);
    if (!dt.isValid())
        return {};
    dt.setTimeZone(QTimeZone::UTC);
    return dt;
}

QByteArray formatHttpDate(const QDateTime& time)
{
    return QLocale::c()
        .toString(time.toUTC(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"))
        .toLatin1();
}

} // namespace qbucket::s3
