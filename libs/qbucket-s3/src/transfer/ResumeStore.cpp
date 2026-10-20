#include <qbucket/s3/transfer/JsonFileResumeStore.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>

#include <map>

namespace qbucket::s3::transfer {

ResumeStore::~ResumeStore() = default;

bool isValidTransferId(const QString& id)
{
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return valid.match(id).hasMatch();
}

namespace {

Error localError(const QString& message)
{
    return Error::make(ErrorCategory::LocalIo, message);
}

QString directionName(Direction d)
{
    switch (d) {
    case Direction::Upload: return QStringLiteral("upload");
    case Direction::Download: return QStringLiteral("download");
    case Direction::Copy: return QStringLiteral("copy");
    }
    return QStringLiteral("upload");
}

Direction directionFrom(const QString& name)
{
    if (name == QLatin1String("download"))
        return Direction::Download;
    if (name == QLatin1String("copy"))
        return Direction::Copy;
    return Direction::Upload;
}

QJsonObject toJson(const TransferRecord& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), 1);
    o.insert(QStringLiteral("id"), r.id);
    o.insert(QStringLiteral("direction"), directionName(r.direction));
    o.insert(QStringLiteral("account"), r.account);
    o.insert(QStringLiteral("bucket"), r.bucket);
    o.insert(QStringLiteral("key"), r.key);
    o.insert(QStringLiteral("versionId"), r.versionId);
    o.insert(QStringLiteral("uploadId"), r.uploadId);
    o.insert(QStringLiteral("localPath"), r.localPath);
    o.insert(QStringLiteral("finalPath"), r.finalPath);
    o.insert(QStringLiteral("sourceBucket"), r.sourceBucket);
    o.insert(QStringLiteral("sourceKey"), r.sourceKey);
    o.insert(QStringLiteral("size"), QString::number(r.size));   // exact beyond 2^53
    o.insert(QStringLiteral("localModified"), r.localModified.toUTC().toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("etag"), r.etag);
    o.insert(QStringLiteral("partSize"), QString::number(r.partSize));
    o.insert(QStringLiteral("partCount"), r.partCount);
    o.insert(QStringLiteral("extra"), r.extra);
    return o;
}

TransferRecord recordFrom(const QJsonObject& o)
{
    TransferRecord r;
    r.id = o.value(QStringLiteral("id")).toString();
    r.direction = directionFrom(o.value(QStringLiteral("direction")).toString());
    r.account = o.value(QStringLiteral("account")).toString();
    r.bucket = o.value(QStringLiteral("bucket")).toString();
    r.key = o.value(QStringLiteral("key")).toString();
    r.versionId = o.value(QStringLiteral("versionId")).toString();
    r.uploadId = o.value(QStringLiteral("uploadId")).toString();
    r.localPath = o.value(QStringLiteral("localPath")).toString();
    r.finalPath = o.value(QStringLiteral("finalPath")).toString();
    r.sourceBucket = o.value(QStringLiteral("sourceBucket")).toString();
    r.sourceKey = o.value(QStringLiteral("sourceKey")).toString();
    r.size = o.value(QStringLiteral("size")).toString().toLongLong();
    r.localModified = QDateTime::fromString(o.value(QStringLiteral("localModified")).toString(), Qt::ISODateWithMs);
    r.etag = o.value(QStringLiteral("etag")).toString();
    r.partSize = o.value(QStringLiteral("partSize")).toString().toLongLong();
    r.partCount = o.value(QStringLiteral("partCount")).toInt();
    r.extra = o.value(QStringLiteral("extra")).toObject();
    return r;
}

QByteArray partLine(const PartRecord& p)
{
    QJsonObject o;
    o.insert(QStringLiteral("n"), p.partNumber);
    o.insert(QStringLiteral("offset"), QString::number(p.offset));
    o.insert(QStringLiteral("length"), QString::number(p.length));
    o.insert(QStringLiteral("etag"), p.etag);
    o.insert(QStringLiteral("md5"), QString::fromLatin1(p.md5.toHex()));
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace

JsonFileResumeStore::JsonFileResumeStore(QString directory)
    : m_dir(std::move(directory))
{
}

QString JsonFileResumeStore::recordPath(const QString& id) const
{
    return QDir(m_dir).filePath(id + QStringLiteral(".json"));
}

QString JsonFileResumeStore::partsPath(const QString& id) const
{
    return QDir(m_dir).filePath(id + QStringLiteral(".parts"));
}

Result<void> JsonFileResumeStore::writeRecord(const TransferRecord& record) const
{
    if (!isValidTransferId(record.id))
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("invalid transfer id"));
    if (!QDir().mkpath(m_dir))
        return localError(QStringLiteral("cannot create %1").arg(m_dir));
    QSaveFile f(recordPath(record.id));
    if (!f.open(QIODevice::WriteOnly))
        return localError(QStringLiteral("%1: %2").arg(f.fileName(), f.errorString()));
    f.write(QJsonDocument(toJson(record)).toJson(QJsonDocument::Indented));
    if (!f.commit())
        return localError(QStringLiteral("%1: %2").arg(f.fileName(), f.errorString()));
    return {};
}

Result<void> JsonFileResumeStore::begin(const TransferRecord& record)
{
    QMutexLocker lock(&m_mutex);
    if (Result<void> r = writeRecord(record); !r)
        return r;
    QFile parts(partsPath(record.id));
    if (!parts.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return localError(QStringLiteral("%1: %2").arg(parts.fileName(), parts.errorString()));
    return {};
}

Result<void> JsonFileResumeStore::update(const TransferRecord& record)
{
    QMutexLocker lock(&m_mutex);
    if (!QFile::exists(recordPath(record.id)))
        return Error::make(ErrorCategory::NotFound, QStringLiteral("no transfer %1").arg(record.id));
    return writeRecord(record);
}

Result<void> JsonFileResumeStore::addPart(const QString& id, const PartRecord& part)
{
    if (!isValidTransferId(id))
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("invalid transfer id"));
    QMutexLocker lock(&m_mutex);
    if (!QFile::exists(recordPath(id)))
        return Error::make(ErrorCategory::NotFound, QStringLiteral("no transfer %1").arg(id));
    QFile f(partsPath(id));
    if (!f.open(QIODevice::ReadWrite | QIODevice::Append))
        return localError(QStringLiteral("%1: %2").arg(f.fileName(), f.errorString()));
    QByteArray line = partLine(part);
    // After a crash the file may end in a torn line: start a fresh one, so
    // this record isn't glued to it.
    if (f.size() > 0) {
        char last = '\n';
        if (f.seek(f.size() - 1) && f.read(&last, 1) == 1 && last != '\n')
            line.prepend('\n');
    }
    if (f.write(line) != line.size() || !f.flush())
        return localError(QStringLiteral("%1: %2").arg(f.fileName(), f.errorString()));
    return {};
}

Result<ResumeState> JsonFileResumeStore::load(const QString& id) const
{
    if (!isValidTransferId(id))
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("invalid transfer id"));
    QMutexLocker lock(&m_mutex);
    QFile header(recordPath(id));
    if (!header.open(QIODevice::ReadOnly))
        return Error::make(ErrorCategory::NotFound, QStringLiteral("no transfer %1").arg(id));
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(header.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return Error::make(ErrorCategory::Protocol, QStringLiteral("%1: %2").arg(header.fileName(), error.errorString()));
    ResumeState state;
    state.record = recordFrom(doc.object());
    if (state.record.id != id)
        return Error::make(ErrorCategory::Protocol, QStringLiteral("%1 names another transfer").arg(header.fileName()));

    std::map<int, PartRecord> parts;
    QFile f(partsPath(id));
    if (f.open(QIODevice::ReadOnly)) {
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty())
                continue;
            const QJsonDocument d = QJsonDocument::fromJson(line);
            if (!d.isObject())
                continue;   // a torn last line from a crash
            const QJsonObject o = d.object();
            PartRecord p;
            p.partNumber = o.value(QStringLiteral("n")).toInt();
            p.offset = o.value(QStringLiteral("offset")).toString().toLongLong();
            p.length = o.value(QStringLiteral("length")).toString().toLongLong();
            p.etag = o.value(QStringLiteral("etag")).toString();
            p.md5 = QByteArray::fromHex(o.value(QStringLiteral("md5")).toString().toLatin1());
            if (p.partNumber <= 0 || p.md5.size() != 16)
                continue;
            parts[p.partNumber] = p;
        }
    }
    for (const auto& [n, p] : parts)
        state.parts.append(p);
    return state;
}

QStringList JsonFileResumeStore::ids() const
{
    QMutexLocker lock(&m_mutex);
    QStringList out;
    const QStringList files = QDir(m_dir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        const QString id = f.chopped(5);
        if (isValidTransferId(id))
            out.append(id);
    }
    return out;
}

Result<void> JsonFileResumeStore::remove(const QString& id)
{
    if (!isValidTransferId(id))
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("invalid transfer id"));
    QMutexLocker lock(&m_mutex);
    QFile::remove(partsPath(id));
    if (QFile::exists(recordPath(id)) && !QFile::remove(recordPath(id)))
        return localError(QStringLiteral("cannot remove %1").arg(recordPath(id)));
    return {};
}

} // namespace qbucket::s3::transfer
