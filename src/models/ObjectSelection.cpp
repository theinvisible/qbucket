#include "models/ObjectSelection.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

namespace qbucket {

namespace {

// Tells this process's selections from another QBucket's.
QString instanceId()
{
    static const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return id;
}

} // namespace

QMimeData* ObjectSelection::toMimeData() const
{
    QJsonArray list;
    QStringList uris;
    for (const Item& i : items) {
        list.append(QJsonObject{{QStringLiteral("key"), i.key}, {QStringLiteral("folder"), i.folder}});
        uris.append(QStringLiteral("s3://%1/%2").arg(bucket, i.key));
    }
    const QJsonObject o{
        {QStringLiteral("v"), 1},
        {QStringLiteral("instance"), instanceId()},
        {QStringLiteral("account"), account.toString(QUuid::WithoutBraces)},
        {QStringLiteral("bucket"), bucket},
        {QStringLiteral("base"), base},
        {QStringLiteral("intent"), intent == Intent::Cut ? QStringLiteral("cut") : QStringLiteral("copy")},
        {QStringLiteral("items"), list},
    };
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kMimeType), QJsonDocument(o).toJson(QJsonDocument::Compact));
    mime->setText(uris.join(u'\n'));
    return mime;
}

std::optional<ObjectSelection> ObjectSelection::fromMimeData(const QMimeData* mime)
{
    if (!mime || !mime->hasFormat(QString::fromLatin1(kMimeType)))
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(mime->data(QString::fromLatin1(kMimeType))).object();
    if (o.value(QStringLiteral("v")).toInt() != 1 || o.value(QStringLiteral("instance")).toString() != instanceId())
        return std::nullopt;
    ObjectSelection s;
    s.account = QUuid(o.value(QStringLiteral("account")).toString());
    s.bucket = o.value(QStringLiteral("bucket")).toString();
    s.base = o.value(QStringLiteral("base")).toString();
    s.intent = o.value(QStringLiteral("intent")).toString() == QLatin1String("cut") ? Intent::Cut : Intent::Copy;
    for (const auto& v : o.value(QStringLiteral("items")).toArray()) {
        const QJsonObject i = v.toObject();
        const QString key = i.value(QStringLiteral("key")).toString();
        if (key.isEmpty() || !key.startsWith(s.base))
            return std::nullopt;
        s.items.append({key, i.value(QStringLiteral("folder")).toBool()});
    }
    if (s.account.isNull() || s.bucket.isEmpty() || s.items.isEmpty())
        return std::nullopt;
    return s;
}

} // namespace qbucket
