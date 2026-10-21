#include "accounts/LocationStore.h"

#include "diagnostics/Logging.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace qbucket {
namespace {

constexpr int kSaveDelayMs = 300;

QString defaultPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/locations.json");
}

QJsonObject locationJson(const Location& l)
{
    QJsonObject o;
    o.insert(QStringLiteral("account"), l.account.toString(QUuid::WithoutBraces));
    o.insert(QStringLiteral("bucket"), l.bucket);
    o.insert(QStringLiteral("prefix"), l.prefix);
    return o;
}

Location locationFrom(const QJsonObject& o)
{
    Location l;
    l.account = QUuid::fromString(o.value(QStringLiteral("account")).toString());
    l.bucket = o.value(QStringLiteral("bucket")).toString();
    l.prefix = o.value(QStringLiteral("prefix")).toString();
    if (!l.prefix.isEmpty() && !l.prefix.endsWith(u'/'))
        l.prefix.clear();   // hand-edited: never a key as a folder
    return l;
}

} // namespace

LocationStore::LocationStore(QObject* parent)
    : LocationStore(defaultPath(), parent)
{
}

LocationStore::LocationStore(const QString& filePath, QObject* parent)
    : QObject(parent),
      m_path(filePath),
      m_saveTimer(new QTimer(this))
{
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, &LocationStore::save_);
    load_();
}

LocationStore::~LocationStore()
{
    flush();
}

std::optional<Bookmark> LocationStore::bookmark(const QUuid& id) const
{
    for (const Bookmark& b : m_bookmarks) {
        if (b.id == id)
            return b;
    }
    return std::nullopt;
}

QString LocationStore::defaultName(const Location& location)
{
    const QStringList segments = location.segments();
    for (auto it = segments.crbegin(); it != segments.crend(); ++it) {
        if (!it->isEmpty())
            return *it;
    }
    return location.bucket;
}

Bookmark LocationStore::addBookmark(const QString& name, const Location& location)
{
    Bookmark b;
    b.id = QUuid::createUuid();
    b.name = name.trimmed().isEmpty() ? defaultName(location) : name.trimmed();
    b.location = location;
    m_bookmarks.append(b);
    save_();
    emit bookmarksChanged();
    return b;
}

bool LocationStore::renameBookmark(const QUuid& id, const QString& name)
{
    const QString next = name.trimmed();
    for (Bookmark& b : m_bookmarks) {
        if (b.id != id)
            continue;
        if (next.isEmpty() || b.name == next)
            return false;
        b.name = next;
        save_();
        emit bookmarksChanged();
        return true;
    }
    return false;
}

bool LocationStore::removeBookmark(const QUuid& id)
{
    const qsizetype removed = m_bookmarks.removeIf([&id](const Bookmark& b) { return b.id == id; });
    if (removed == 0)
        return false;
    save_();
    emit bookmarksChanged();
    return true;
}

bool LocationStore::moveBookmark(const QUuid& id, int index)
{
    const auto it = std::find_if(m_bookmarks.cbegin(), m_bookmarks.cend(), [&id](const Bookmark& b) { return b.id == id; });
    if (it == m_bookmarks.cend())
        return false;
    const qsizetype from = it - m_bookmarks.cbegin();
    const qsizetype to = std::clamp<qsizetype>(index, 0, m_bookmarks.size() - 1);
    if (from == to)
        return false;
    m_bookmarks.move(from, to);
    save_();
    emit bookmarksChanged();
    return true;
}

void LocationStore::visited(const Location& location)
{
    if (!location.hasBucket())
        return;
    if (!m_recent.isEmpty() && m_recent.first() == location)
        return;
    m_recent.removeAll(location);
    m_recent.prepend(location);
    while (m_recent.size() > kMaxRecent)
        m_recent.removeLast();
    scheduleSave_();
    emit recentChanged();
}

void LocationStore::clearRecent()
{
    if (m_recent.isEmpty())
        return;
    m_recent.clear();
    save_();
    emit recentChanged();
}

void LocationStore::forgetAccount(const QUuid& account)
{
    const qsizetype marks = m_bookmarks.removeIf([&account](const Bookmark& b) { return b.location.account == account; });
    const qsizetype recent = m_recent.removeIf([&account](const Location& l) { return l.account == account; });
    if (marks == 0 && recent == 0)
        return;
    save_();
    if (marks > 0)
        emit bookmarksChanged();
    if (recent > 0)
        emit recentChanged();
}

void LocationStore::forgetBucket(const QUuid& account, const QString& bucket)
{
    const auto in = [&](const Location& l) { return l.account == account && l.bucket == bucket; };
    const qsizetype marks = m_bookmarks.removeIf([&in](const Bookmark& b) { return in(b.location); });
    const qsizetype recent = m_recent.removeIf(in);
    if (marks == 0 && recent == 0)
        return;
    save_();
    if (marks > 0)
        emit bookmarksChanged();
    if (recent > 0)
        emit recentChanged();
}

void LocationStore::flush()
{
    if (m_saveTimer->isActive())
        save_();
}

void LocationStore::scheduleSave_()
{
    m_saveTimer->start();
}

void LocationStore::load_()
{
    QFile f(m_path);
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly)) {
        m_loadProblem = f.errorString();
        qCWarning(lcApp).noquote() << "locations: cannot read" << m_path << m_loadProblem;
        return;
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (!doc.isObject()) {
        m_loadProblem = error.errorString();
        qCWarning(lcApp).noquote() << "locations: ignoring a damaged" << m_path << m_loadProblem;
        return;
    }
    const QJsonObject root = doc.object();
    for (const auto& v : root.value(QStringLiteral("bookmarks")).toArray()) {
        const QJsonObject o = v.toObject();
        Bookmark b;
        b.id = QUuid::fromString(o.value(QStringLiteral("id")).toString());
        b.name = o.value(QStringLiteral("name")).toString().trimmed();
        b.location = locationFrom(o);
        if (b.id.isNull() || b.name.isEmpty() || !b.location.isValid() || bookmark(b.id))
            continue;
        m_bookmarks.append(b);
    }
    for (const auto& v : root.value(QStringLiteral("recent")).toArray()) {
        const Location l = locationFrom(v.toObject());
        if (l.hasBucket() && !m_recent.contains(l) && m_recent.size() < kMaxRecent)
            m_recent.append(l);
    }
}

void LocationStore::save_()
{
    m_saveTimer->stop();
    QJsonArray marks;
    for (const Bookmark& b : m_bookmarks) {
        QJsonObject o = locationJson(b.location);
        o.insert(QStringLiteral("id"), b.id.toString(QUuid::WithoutBraces));
        o.insert(QStringLiteral("name"), b.name);
        marks.append(o);
    }
    QJsonArray recent;
    for (const Location& l : m_recent)
        recent.append(locationJson(l));
    QJsonObject root;
    root.insert(QStringLiteral("format"), 1);
    root.insert(QStringLiteral("bookmarks"), marks);
    root.insert(QStringLiteral("recent"), recent);

    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0 || !f.commit())
        qCWarning(lcApp).noquote() << "locations: cannot write" << m_path << f.errorString();
}

} // namespace qbucket
