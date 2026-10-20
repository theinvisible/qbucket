#include "controllers/TransferJobStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace qbucket {

namespace {

QString kindName(Job::TransferKind k)
{
    switch (k) {
    case Job::TransferKind::Upload: return QStringLiteral("upload");
    case Job::TransferKind::Download: return QStringLiteral("download");
    case Job::TransferKind::None: break;
    }
    return QStringLiteral("none");
}

Job::TransferKind kindFrom(const QString& name)
{
    if (name == QLatin1String("upload"))
        return Job::TransferKind::Upload;
    if (name == QLatin1String("download"))
        return Job::TransferKind::Download;
    return Job::TransferKind::None;
}

QJsonObject toJson(const TransferJobStore::Record& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), 1);
    o.insert(QStringLiteral("id"), r.id);
    o.insert(QStringLiteral("kind"), kindName(r.kind));
    o.insert(QStringLiteral("account"), r.account.toString(QUuid::WithoutBraces));
    o.insert(QStringLiteral("bucket"), r.bucket);
    o.insert(QStringLiteral("title"), r.title);
    o.insert(QStringLiteral("prefix"), r.prefix);
    o.insert(QStringLiteral("target"), r.target);
    o.insert(QStringLiteral("policy"), int(r.policy));
    o.insert(QStringLiteral("items"), QString::number(r.items));
    o.insert(QStringLiteral("bytes"), QString::number(r.bytes));
    o.insert(QStringLiteral("started"), r.started.toUTC().toString(Qt::ISODate));
    o.insert(QStringLiteral("suspended"), r.suspended);
    return o;
}

TransferJobStore::Record fromJson(const QJsonObject& o)
{
    TransferJobStore::Record r;
    r.id = o.value(QStringLiteral("id")).toString();
    r.kind = kindFrom(o.value(QStringLiteral("kind")).toString());
    r.account = QUuid::fromString(o.value(QStringLiteral("account")).toString());
    r.bucket = o.value(QStringLiteral("bucket")).toString();
    r.title = o.value(QStringLiteral("title")).toString();
    r.prefix = o.value(QStringLiteral("prefix")).toString();
    r.target = o.value(QStringLiteral("target")).toString();
    r.policy = Job::ConflictPolicy(std::clamp(o.value(QStringLiteral("policy")).toInt(), 0, 2));
    r.items = o.value(QStringLiteral("items")).toString().toLongLong();
    r.bytes = o.value(QStringLiteral("bytes")).toString().toLongLong();
    r.started = QDateTime::fromString(o.value(QStringLiteral("started")).toString(), Qt::ISODate);
    r.suspended = o.value(QStringLiteral("suspended")).toBool();
    return r;
}

} // namespace

TransferJobStore::TransferJobStore(const QString& directory, QObject* parent)
    : QObject(parent),
      m_dir(directory)
{
    m_io.setMaxThreadCount(1);
    m_io.setObjectName(QStringLiteral("TransferJobStore"));
    m_flush.setSingleShot(true);
    m_flush.setInterval(250);
    connect(&m_flush, &QTimer::timeout, this, &TransferJobStore::flushDone_);
}

TransferJobStore::~TransferJobStore()
{
    flushDone_();
    m_io.waitForDone();
}

QString TransferJobStore::planPath(const QString& id) const
{
    return QDir(m_dir).filePath(id + QStringLiteral(".plan"));
}

void TransferJobStore::save(const Record& record)
{
    const QString path = QDir(m_dir).filePath(record.id + QStringLiteral(".json"));
    const QByteArray json = QJsonDocument(toJson(record)).toJson(QJsonDocument::Indented);
    const QString dir = m_dir;
    m_io.start([dir, path, json] {
        QDir().mkpath(dir);
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(json);
            f.commit();
        }
    });
}

void TransferJobStore::markDone(const QString& id, qint64 ordinal)
{
    QByteArray& lines = m_pendingDone[id];
    lines += QByteArray::number(ordinal) + '\n';
    if (lines.size() >= 4096)
        flushDone_();
    else if (!m_flush.isActive())
        m_flush.start();
}

void TransferJobStore::flushDone_()
{
    m_flush.stop();
    const QHash<QString, QByteArray> pending = std::exchange(m_pendingDone, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        const QString path = QDir(m_dir).filePath(it.key() + QStringLiteral(".done"));
        const QByteArray& lines = it.value();
        m_io.start([path, lines] {
            QFile f(path);
            if (f.open(QIODevice::Append))
                f.write(lines);
        });
    }
}

void TransferJobStore::remove(const QString& id)
{
    m_pendingDone.remove(id);
    const QDir dir(m_dir);
    const QStringList files = {dir.filePath(id + QStringLiteral(".json")), dir.filePath(id + QStringLiteral(".done")),
                               planPath(id)};
    m_io.start([files] {
        for (const QString& f : files)
            QFile::remove(f);
    });
}

template <typename T>
void TransferJobStore::post_(QObject* context, std::function<T()> work, std::function<void(const T&)> done)
{
    auto* relay = new QObject;
    relay->moveToThread(context->thread());
    const QPointer<QObject> guard(context);
    m_io.start([relay, guard, work = std::move(work), done = std::move(done)] {
        T result = work();
        QMetaObject::invokeMethod(relay, [relay, guard, done, result] {
            if (guard)
                done(result);
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

void TransferJobStore::load(QObject* context, std::function<void(const QList<Record>&)> done)
{
    flushDone_();
    const QString dirPath = m_dir;
    post_<QList<Record>>(context, [dirPath] {
        QList<Record> out;
        const QDir dir(dirPath);
        for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (!doc.isObject())
                continue;
            Record r = fromJson(doc.object());
            // A record without its plan can't be continued.
            if (r.id.isEmpty() || r.kind == Job::TransferKind::None
                || !QFile::exists(dir.filePath(r.id + QStringLiteral(".plan"))))
                continue;
            out.append(r);
        }
        return out;
    }, std::move(done));
}

void TransferJobStore::loadDone(const QString& id, QObject* context, std::function<void(const QSet<qint64>&)> done)
{
    flushDone_();
    const QString path = QDir(m_dir).filePath(id + QStringLiteral(".done"));
    post_<QSet<qint64>>(context, [path] {
        QSet<qint64> out;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            while (!f.atEnd()) {
                bool ok = false;
                const qint64 n = f.readLine().trimmed().toLongLong(&ok);
                if (ok)
                    out.insert(n);
            }
        }
        return out;
    }, std::move(done));
}

bool TransferJobStore::waitForWrites(int timeoutMs)
{
    flushDone_();
    return m_io.waitForDone(timeoutMs);
}

} // namespace qbucket
